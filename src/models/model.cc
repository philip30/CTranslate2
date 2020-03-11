#include "ctranslate2/models/model.h"

#include <fstream>

#include "ctranslate2/models/transformer.h"
#include "ctranslate2/utils.h"

namespace ctranslate2 {
  namespace models {

    template <typename T>
    T consume(std::istream& in) {
      T val;
      in.read(reinterpret_cast<char*>(&val), sizeof (T));
      return val;
    }

    template <typename T>
    T* consume(std::istream& in, size_t n, T* data = nullptr) {
      if (n == 0)
        return nullptr;
      if (data == nullptr)
        data = new T[n];
      in.read(reinterpret_cast<char*>(data), n * sizeof (T));
      return data;
    }

    template<>
    std::string consume(std::istream& in) {
      const auto str_length = consume<uint16_t>(in);
      const auto c_str = consume<char>(in, str_length);
      std::string str(c_str);
      delete [] c_str;
      return str;
    }

    static void move_variables_to_device(std::unordered_map<std::string, StorageView>& variables,
                                         const Device device) {
      for (auto& pair : variables) {
        StorageView& variable = pair.second;
        if (!variable.is_scalar() && variable.device() != device) {
          StorageView variable_device = variable.to(device);
          swap(variable, variable_device);
        }
      }
    }

    static void move_variables(std::unordered_map<std::string, StorageView>& variables,
                               const Device src_device, const int src_device_index,
                               const Device dst_device, const int dst_device_index) {
      if (variables.empty())
        return;
      if (src_device == dst_device && src_device_index == dst_device_index)
        return;

      // Move variables back to the CPU device.
      if (src_device != Device::CPU) {
        ScopedDeviceSetter scoped_device_setter(src_device, src_device_index);
        move_variables_to_device(variables, Device::CPU);
      }

      // Move variables to the destination device.
      if (dst_device != Device::CPU) {
        ScopedDeviceSetter scoped_device_setter(dst_device, dst_device_index);
        move_variables_to_device(variables, dst_device);
      }
    }


    Model::Model(const std::string& path, size_t spec_revision)
      : _spec_revision(spec_revision) {
      try {
        _shared_vocabulary.reset(new Vocabulary(path + "/shared_vocabulary.txt"));
      } catch (std::exception&) {
        _source_vocabulary.reset(new Vocabulary(path + "/source_vocabulary.txt"));
        _target_vocabulary.reset(new Vocabulary(path + "/target_vocabulary.txt"));
      }
      _vocabulary_map.reset(new VocabularyMap(path + "/vmap.txt", get_target_vocabulary()));
    }

    size_t Model::current_spec_revision() const {
      return 1;
    }

    Device Model::device() const {
      return _device;
    }

    int Model::device_index() const
    {
      return _device_index;
    }

    ComputeType Model::compute_type() const {
      return _compute_type;
    }

    void Model::set_device(const Device device, const int index) {
      move_variables(_variable_index, _device, _device_index, device, index);
      _device = device;
      _device_index = index;
    }

    void Model::set_compute_type(ComputeType type) {
      _compute_type = type;
    }

    ScopedDeviceSetter Model::get_scoped_device_setter() const {
      return ScopedDeviceSetter(_device, _device_index);
    }

    const Vocabulary& Model::get_source_vocabulary() const {
      return _shared_vocabulary ? *_shared_vocabulary : *_source_vocabulary;
    }

    const Vocabulary& Model::get_target_vocabulary() const {
      return _shared_vocabulary ? *_shared_vocabulary : *_target_vocabulary;
    }

    const VocabularyMap& Model::get_vocabulary_map() const {
      return *_vocabulary_map;
    }

    const StorageView* Model::get_variable_if_exists(const std::string& name) const {
      auto alias_it = _variable_alias.find(name);
      const auto variable_name = alias_it != _variable_alias.end() ? alias_it->second : name;
      auto it = _variable_index.find(variable_name);
      if (it == _variable_index.end())
        return nullptr;
      return &it->second;
    }

    const StorageView& Model::get_variable(const std::string& name) const {
      const auto* var = get_variable_if_exists(name);
      if (var == nullptr)
        throw std::out_of_range("variable " + name + " not found");
      return *var;
    }

    const std::unordered_map<std::string, StorageView>& Model::get_variables() const {
      return _variable_index;
    }

    bool Model::get_flag_with_default(const std::string& name, bool default_value) const {
      return get_attribute_with_default(name, static_cast<int8_t>(default_value));
    }

    void Model::register_variable(const std::string& name, StorageView& variable) {
      _variable_index.emplace(std::piecewise_construct,
                              std::forward_as_tuple(name),
                              std::forward_as_tuple(std::move(variable)));
    }

    void Model::register_variable_alias(const std::string& alias,
                                        const std::string& variable_name) {
      _variable_alias.emplace(alias, variable_name);
      // Also alias the quantization scale that could be associated to variable_name.
      _variable_alias.emplace(alias + "_scale", variable_name + "_scale");
    }

    StorageView* Model::get_scale(const std::string& scale_name, DataType dataType) {
      StorageView* scale = nullptr;
      auto scale_it = _variable_index.find(scale_name);
      if (scale_it != _variable_index.end())
        scale = &scale_it->second;

      // Compatibility with int16 models without a saved scale.
      if (scale == nullptr) {
        if (dataType == DataType::INT16) {
          StorageView compat_scale(ops::Quantize::default_int16_scale);
          Model::register_variable(scale_name, compat_scale);
          scale = &_variable_index.at(scale_name);
        }
      }

      if (!scale) {
        throw std::invalid_argument("Model data is uncompatible. scale is NULL.");
      }

      return scale;
    }

    void
    Model::convert_data_if_need(bool support_int8,
                                bool support_int16,
                                const std::string& name,
                                StorageView& variable,
                                std::vector<std::pair<std::string, StorageView>>& variables_to_add,
                                std::vector<std::string>& variables_to_remove) {
      const bool is_int8 = variable.dtype() == DataType::INT8;
      const bool is_int16 = variable.dtype() == DataType::INT16;
      const bool is_float = variable.dtype() == DataType::FLOAT;

      // Use the same quantization logic as in model_spec.py.
      const ops::Quantize quantize_op(/*int16_scale_type=*/ops::Quantize::ScaleType::PER_LAYER);
      const ops::Dequantize dequantize_op{};

      std::string scale_name = name + "_scale";
      if (_compute_type == ComputeType::DEFAULT) {
        if (is_int8 || is_int16) {
          StorageView* scale = get_scale(scale_name, variable.dtype());

          // If quantized variables are not supported, fallback to float32.
          if ((is_int16 && !support_int16) || (is_int8 && !support_int8)) {
            StorageView variable_float;
            dequantize_op(variable, *scale, variable_float);
            swap(variable, variable_float);

            // However, if int16 is supported and we came from int8, quantize to int16.
            if (is_int8 && support_int16) {
              StorageView variable_int16(DataType::INT16);
              quantize_op(variable, variable_int16, *scale);
              swap(variable, variable_int16);
            } else { // is_int16 or !support_int16
              variables_to_remove.emplace_back(std::move(scale_name));
            }
          }
        }
      } else if (_compute_type == ComputeType::FLOAT) {

        if (is_float) {
          // do nothing
        } else { // is_int8 || is_int16
          StorageView* scale = get_scale(scale_name, variable.dtype());

          // fallback to float32
          StorageView variable_float;
          dequantize_op(variable, *scale, variable_float);
          swap(variable, variable_float);
        }

      } else if ((_compute_type == ComputeType::INT16 && is_int16) ||
                (_compute_type == ComputeType::INT8 && is_int8)) {
        // Make & register a scale if the scale is absent
        get_scale(scale_name, variable.dtype());

      } else {
        if (is_float) {

          StorageView scale(DataType::FLOAT);

          // from float32 to int16
          StorageView variable_int(_compute_type == ComputeType::INT16
                                   ? DataType::INT16
                                   : DataType::INT8);
          quantize_op(variable, variable_int, scale);
          swap(variable, variable_int);

          variables_to_add.emplace_back(scale_name, scale);

        } else { // DataType::INT8 or DataType::INT16
          StorageView* scale = get_scale(scale_name, variable.dtype());

          // from int8 to float32 firstly
          StorageView variable_float;
          dequantize_op(variable, *scale, variable_float);
          swap(variable, variable_float);

          // from float to int
          StorageView variable_int(_compute_type == ComputeType::INT8
                                   ? DataType::INT8
                                   : DataType::INT16);
          quantize_op(variable, variable_int, *scale);
          swap(variable, variable_int);
        }
      }
    }

    void Model::finalize() {
      auto scoped_device_setter = get_scoped_device_setter();

      const bool support_int8 = mayiuse_int8(_device, _device_index);
      const bool support_int16 = mayiuse_int16(_device, _device_index);

      std::vector<std::string> variables_to_remove;
      std::vector<std::pair<std::string, StorageView>> variables_to_add;

      // Make sure CPU supports the demanded type
      if ((_compute_type == ComputeType::INT8) && (!support_int8)) {
        throw std::invalid_argument("Requested int8 compute type, but device doesn't "
                                    "support efficient int8 computation.");
      } else if ((_compute_type == ComputeType::INT16) && (!support_int16)) {
        throw std::invalid_argument("Requested int16 compute type, but device doesn't "
                                    "support efficient int16 computation.");
      }

      for (auto& variable_pair : _variable_index) {
        const auto& name = variable_pair.first;
        auto& variable = variable_pair.second;

        // Only process "weight" variables.
        if (ends_with(name, "weight")) {
          convert_data_if_need(support_int8,
                               support_int16,
                               name,
                               variable,
                               variables_to_add,
                               variables_to_remove);
        }
      }

      // Remove no longer needed variables.
      for (const auto& name : variables_to_remove)
        _variable_index.erase(name);

      // Add needed variables.
      for (auto& variable_pair : variables_to_add)
        _variable_index.emplace(std::move(variable_pair));

      // Second pass to move variables on the target device.
      move_variables_to_device(_variable_index, _device);
    }

    static DataType get_dtype_from_item_size(uint8_t item_size) {
      // This is the old (and flawed) logic of resolving the dtype of saved variables.
      switch (item_size) {
      case 4:
        return DataType::FLOAT;
      case 2:
        return DataType::INT16;
      case 1:
        return DataType::INT8;
      default:
        throw std::runtime_error("unknown data type of width " + std::to_string(item_size));
      }
    }

    static Model* create_model(const std::string& path,
                               const std::string& spec,
                               size_t spec_revision) {
      Model* model = nullptr;

      // Empty spec name, TransformerBase, and TransformerBig are there for backward
      // compatibility. Now all Transformer variants are saved under TransformerSpec.

      if (spec.empty() || spec == "TransformerBase")
        model = new TransformerModel(path, spec_revision, /*num_heads=*/8);
      else if (spec == "TransformerBig")
        model = new TransformerModel(path, spec_revision, /*num_heads=*/16);
      else if (spec == "TransformerSpec")
        model = new TransformerModel(path, spec_revision);
      else
        throw std::invalid_argument("Unsupported model spec " + spec);

      return model;
    }

    static void check_version(const size_t saved_version,
                              const size_t current_version,
                              const std::string& version_type) {
      if (saved_version > current_version)
        throw std::runtime_error("Unsupported model " + version_type
                                 + ". This executable supports models with " + version_type + " v"
                                 + std::to_string(current_binary_version)
                                 + " or below, but the model has " + version_type + " v"
                                 + std::to_string(saved_version)
                                 + ". This usually means that the model was generated by a later "
                                 + "version of CTranslate2. "
                                 + "(Forward compatibility is not guaranteed.)");
    }

    std::shared_ptr<const Model> Model::load(const std::string& path,
                                             const std::string& device,
                                             int device_index,
                                             const std::string& compute_type) {

      return load(path, str_to_device(device), device_index, str_to_compute_type(compute_type));
    }

    std::shared_ptr<const Model> Model::load(const std::string& path,
                                             Device device,
                                             int device_index,
                                             ComputeType compute_type) {
      const std::string model_path = path + "/model.bin";
      std::ifstream model_file(model_path, std::ios_base::in | std::ios_base::binary);
      if (!model_file.is_open())
        throw std::runtime_error("failed to load the model " + model_path);

      // See the model serialization in python/ctranslate2/specs/model_spec.py.
      const auto binary_version = consume<uint32_t>(model_file);
      check_version(binary_version, current_binary_version, "binary version");

      std::string spec;
      size_t spec_revision;
      if (binary_version >= 2) {
        spec = consume<std::string>(model_file);
        spec_revision = consume<uint32_t>(model_file);
      } else {
        spec_revision = 1;
      }

      Model* model = create_model(path, spec, spec_revision);
      model->set_device(device, device_index);
      model->set_compute_type(compute_type);

      check_version(spec_revision, model->current_spec_revision(), "revision");

      const auto num_variables = consume<uint32_t>(model_file);
      for (uint32_t i = 0; i < num_variables; ++i) {
        const auto name = consume<std::string>(model_file);
        const size_t rank = consume<uint8_t>(model_file);
        const auto* dimensions = consume<uint32_t>(model_file, rank);

        DataType dtype;
        dim_t num_bytes = 0;
        if (binary_version >= 4) {
          const auto type_id = consume<uint8_t>(model_file);
          dtype = static_cast<DataType>(type_id);
          num_bytes = consume<uint32_t>(model_file);
        } else {
          const auto item_size = consume<uint8_t>(model_file);
          dtype = get_dtype_from_item_size(item_size);
          num_bytes = consume<uint32_t>(model_file) * item_size;
        }

        StorageView variable({dimensions, dimensions + rank}, dtype);
        consume<char>(model_file, num_bytes, static_cast<char*>(variable.buffer()));
        model->register_variable(name, variable);

        delete [] dimensions;
      }

      if (binary_version >= 3) {
        const auto num_aliases = consume<uint32_t>(model_file);
        for (uint32_t i = 0; i < num_aliases; ++i) {
          const auto alias = consume<std::string>(model_file);
          const auto variable_name = consume<std::string>(model_file);
          model->register_variable_alias(alias, variable_name);
        }
      }

      model->finalize();
      return std::shared_ptr<Model>(model);
    }

    bool contains_model(const std::string& path) {
      return (
        file_exists(path + "/model.bin")
        && ((file_exists(path + "/source_vocabulary.txt")
             && file_exists(path + "/target_vocabulary.txt"))
            || file_exists(path + "/shared_vocabulary.txt")));
    }

  }
}
