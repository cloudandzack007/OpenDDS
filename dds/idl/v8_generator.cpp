/*
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "v8_generator.h"

#include "be_extern.h"

#include <dds/DCPS/Definitions.h>

#include <utl_identifier.h>

using namespace AstTypeClassification;

void v8_generator::gen_includes()
{
  be_global->add_include("<v8.h>", BE_GlobalData::STREAM_H);
  be_global->add_include("<nan.h>", BE_GlobalData::STREAM_CPP);
  be_global->add_include("<codecvt>", BE_GlobalData::STREAM_CPP);
}

bool v8_generator::gen_enum(AST_Enum*, UTL_ScopedName*,
                            const std::vector<AST_EnumVal*>&, const char*)
{
  return true;
}


namespace {

  std::string ltrim(const std::string& s)
  {
    size_t start = s.find_first_not_of("\n\r\t\f\v ");
    return (start == std::string::npos) ? "" : s.substr(start);
  }

  std::string getV8Type(AST_Type* type,
                        AST_PredefinedType::PredefinedType* pt = 0)
  {
    const Classification cls = classify(type);
    if (cls & (CL_ENUM | CL_STRING)) return "String";
    if (cls & (CL_STRUCTURE | CL_UNION)) return "Object";
    if (cls & (CL_ARRAY | CL_SEQUENCE)) return "Array";
    if (cls & CL_PRIMITIVE) {
      AST_Type* actual = resolveActualType(type);
      AST_PredefinedType* p = dynamic_cast<AST_PredefinedType*>(actual);
      OPENDDS_ASSERT(p);
      if (pt) *pt = p->pt();
      switch (p->pt()) {
      case AST_PredefinedType::PT_char:
      case AST_PredefinedType::PT_wchar:
      case AST_PredefinedType::PT_longlong:  // Can't fully fit in a JS "number"
      case AST_PredefinedType::PT_ulonglong: // Can't fully fit in a JS "number"
        return "String";
      case AST_PredefinedType::PT_boolean:
        return "Boolean";
      case AST_PredefinedType::PT_octet:
#if OPENDDS_HAS_EXPLICIT_INTS
      case AST_PredefinedType::PT_uint8:
      case AST_PredefinedType::PT_int8:
#endif
      case AST_PredefinedType::PT_ushort:
      case AST_PredefinedType::PT_short:
      case AST_PredefinedType::PT_ulong:
      case AST_PredefinedType::PT_long:
        return "Integer";
      default:
        return "Number";
      }
    }
    return "";
  }

  bool builtInSeq(AST_Type* type)
  {
    const std::string name = scoped(type->name());
    static const char PRE[] = " ::CORBA::";
    if (std::strncmp(name.c_str(), PRE, sizeof(PRE) - 1)) return false;
    return name.rfind("Seq") == name.size() - 3;
  }

  void gen_copyto(const char* tgt, const char* src, AST_Type* type,
                  const char* prop, std::ostream& strm = be_global->impl_)
  {
    const Classification cls = classify(type);
    AST_PredefinedType::PredefinedType pt = AST_PredefinedType::PT_void;
    const std::string v8Type = getV8Type(type, &pt),
      propName = std::string("Nan::Set(") + tgt + ", " + prop + ", ";

    if (cls & CL_SCALAR) {
      std::string prefix, suffix, postNew;

      if (cls & CL_ENUM) {
        const std::string underscores =
          dds_generator::scoped_helper(type->name(), "_"),
          array = "gen_" + underscores + "_names";
        prefix = '(' + std::string(src) + " >= " + array + "_size) ? \"<<invalid>>\" : " + array + "[static_cast<int>(";
        suffix = ")]";
        postNew = ".ToLocalChecked()";

      } else if (cls & CL_STRING) {
        if (!std::strstr(src, "()")) {
          suffix = ".in()";
        }
        if (cls & CL_WIDE) {
          strm <<
            "  {\n"
            "    const size_t len = ACE_OS::strlen(" << src << suffix << ");\n"
            "    uint16_t* const str = new uint16_t[len + 1];\n"
            "    for (size_t i = 0; i <= len; ++i) {\n"
            "      str[i] = " << src << "[i];\n"
            "    }\n"
            "    " << propName << "Nan::New(str).ToLocalChecked());\n"
            "    delete[] str;\n"
            "  }\n";
          return;
        }
        postNew = ".ToLocalChecked()";

      } else if (pt == AST_PredefinedType::PT_char) {
        strm <<
          "  {\n"
          "    const char str[] = {" << src << ", 0};\n"
          "    " << propName << "Nan::New(str).ToLocalChecked());\n"
          "  }\n";
        return;

      } else if (pt == AST_PredefinedType::PT_wchar) {
        strm <<
          "  {\n"
          "    const uint16_t str[] = {" << src << ", 0};\n"
          "    " << propName << "Nan::New(str).ToLocalChecked());\n"
          "  }\n";
        return;

      } else if (pt == AST_PredefinedType::PT_longlong || pt == AST_PredefinedType::PT_ulonglong) {
        prefix = "std::to_string(";
        suffix = ").c_str()";
        postNew = ".ToLocalChecked()";

      } else if (v8Type == "Number") {
        prefix = "static_cast<double>(";
        suffix = ")";
      }

      strm <<
        "  " << propName << "Nan::New(" << prefix << src << suffix << ")"
             << postNew << ");\n";

    } else if ((cls & CL_SEQUENCE) && builtInSeq(type)) {
      AST_Type* real_type = resolveActualType(type);
      AST_Sequence* seq = dynamic_cast<AST_Sequence*>(real_type);
      OPENDDS_ASSERT(seq);
      AST_Type* elem = seq->base_type();
      strm <<
        "  {\n"
        "    const v8::Local<v8::Array> seq = Nan::New<v8::Array>(" << src
                       << ".length());\n"
        "    for (CORBA::ULong j = 0; j < " << src << ".length(); ++j) {\n";
      gen_copyto("seq", (std::string(src) + "[j]").c_str(), elem, "j");
      strm <<
        "    }\n"
        "    " << propName << "seq" << ");\n"
        "  }\n";

    } else { // struct, sequence, etc.
      strm <<
        "  " << propName << "copyToV8(" << src << "));\n";
    }
  }

  void gen_copyfrom(const char* tgt, const char* src, AST_Type* type,
                    const char* prop, bool prop_index = false, bool fun_assign = false, std::ostream& strm = be_global->impl_)
  {
    const Classification cls = classify(type);
    AST_PredefinedType::PredefinedType pt = AST_PredefinedType::PT_void;
    getV8Type(type, &pt);
    const std::string propName = prop_index ? std::string(tgt) + "[" + prop + "]" : std::string(tgt) + "." + prop,
      assign_prefix = fun_assign ? "(" : " = ",
      assign_suffix = fun_assign ? ")" : "";

    if (cls & CL_SCALAR) {

      std::string ip; // indent prefix

      if (prop_index) {
        strm <<
          "  {\n"
          "    v8::Local<v8::Value> lv = Nan::Get(" << src << ", " << prop << ").ToLocalChecked();\n";
        ip = std::string(4, ' ');
      } else {
        strm <<
          "  {\n"
          "    v8::Local<v8::String> field_str = Nan::New(\"" << prop << "\").ToLocalChecked();\n"
          "    if (Nan::Has(" << src << ", field_str).ToChecked()) {\n"
          "      v8::Local<v8::Value> lv = Nan::Get(" << src << ", field_str).ToLocalChecked();\n";
        ip = std::string(6, ' ');
      }

      if (cls & CL_ENUM) {
        const std::string underscores =
          dds_generator::scoped_helper(type->name(), "_"),
          array = "gen_" + underscores + "_names";
        strm <<
          ip << "if (lv->IsNumber()) {\n" <<
          ip << "  v8::Local<v8::Integer> li = Nan::To<v8::Integer>(lv).ToLocalChecked();\n" <<
          ip << "  " << propName << assign_prefix << "static_cast<" << scoped(type->name()) << ">(li->Value())" << assign_suffix << ";\n" <<
          ip << "}\n" <<
          ip << "if (lv->IsString()) {\n" <<
          ip << "  v8::Local<v8::String> ls = Nan::To<v8::String>(lv).ToLocalChecked();\n" <<
          ip << "  std::string ss(*Nan::Utf8String(ls));\n" <<
          ip << "  for (uint32_t i = 0; i < " << array << "_size; ++i) {\n" <<
          ip << "    if (ss == " << array << "[i]) {\n" <<
          ip << "      " << propName << assign_prefix << "static_cast<" << scoped(type->name()) << ">(i)" << assign_suffix << ";\n" <<
          ip << "      break;\n" <<
          ip << "    }\n" <<
          ip << "  }\n" <<
          ip << "}\n";
      } else if (cls & CL_STRING) {
        strm <<
          ip << "if (lv->IsString()) {\n" <<
          ip << "  v8::Local<v8::String> ls = Nan::To<v8::String>(lv).ToLocalChecked();\n";
        if (cls & CL_WIDE) {
          strm <<
            ip << "  std::string ss(*Nan::Utf8String(ls));\n" <<
            ip << "  std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> wconv;\n" <<
            ip << "  std::wstring ws = wconv.from_bytes(ss);\n" <<
            ip << "  " << propName << assign_prefix << "ws.c_str()" << assign_suffix << ";\n";
        } else {
          strm <<
            ip << "  std::string ss(*Nan::Utf8String(ls));\n" <<
            ip << "  " << propName << assign_prefix << "ss.c_str()" << assign_suffix << ";\n";
        }
        strm <<
          ip << "}\n";
      } else if (pt == AST_PredefinedType::PT_char) {
        strm <<
          ip << "if (lv->IsString()) {\n" <<
          ip << "  v8::Local<v8::String> ls = Nan::To<v8::String>(lv).ToLocalChecked();\n" <<
          ip << "  std::string ss(*Nan::Utf8String(ls));\n" <<
          ip << "  const char temp_c = ss[0];\n" <<
          ip << "  " << propName << assign_prefix << "temp_c" << assign_suffix << ";\n" <<
          ip << "}\n";
      } else if (pt == AST_PredefinedType::PT_wchar) {
        strm <<
          ip << "if (lv->IsString()) {\n" <<
          ip << "  v8::Local<v8::String> ls = Nan::To<v8::String>(lv).ToLocalChecked();\n" <<
          ip << "  std::string ss(*Nan::Utf8String(ls));\n" <<
          ip << "  std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> wconv;\n" <<
          ip << "  std::wstring ws = wconv.from_bytes(ss);\n" <<
          ip << "  const wchar temp_wc = ws[0];\n" <<
          ip << "  " << propName << assign_prefix << "temp_wc" << assign_suffix << ";\n" <<
          ip << "}\n";
      } else if (pt == AST_PredefinedType::PT_longlong
              || pt == AST_PredefinedType::PT_ulonglong
              || pt == AST_PredefinedType::PT_octet
              || pt == AST_PredefinedType::PT_ushort
              || pt == AST_PredefinedType::PT_short
              || pt == AST_PredefinedType::PT_ulong
              || pt == AST_PredefinedType::PT_long) {
        const bool is_unsigned = (pt == AST_PredefinedType::PT_ulonglong || pt == AST_PredefinedType::PT_ulong || pt == AST_PredefinedType::PT_ushort || pt == AST_PredefinedType::PT_octet),
          is_longlong = (pt == AST_PredefinedType::PT_ulonglong || pt == AST_PredefinedType::PT_longlong);
        const std::string underscores = dds_generator::scoped_helper(type->name(), "_"),
          temp_type = ltrim(scoped(type->name())),
          temp_name = "temp_" + underscores,
          conv_fun = std::string("std::strto") + (is_unsigned ? "u" : "") + (is_longlong ? "ll" : "l");
        strm <<
          ip << "if (lv->IsString()) {\n" <<
          ip << "  v8::Local<v8::String> ls = Nan::To<v8::String>(lv).ToLocalChecked();\n" <<
          ip << "  std::string ss(*Nan::Utf8String(ls));\n" <<
          ip << "  " << temp_type << " " << temp_name << ";\n" <<
          ip << "  " << temp_name << " = static_cast<" << temp_type << ">(" << conv_fun << "(ss.c_str(), 0, ss.find(\"0x\") != std::string::npos ? 16 : 10));" <<
          ip << "  " << propName << assign_prefix << temp_name << assign_suffix << ";\n" <<
          ip << "} else {\n" <<
          ip << "  Nan::Maybe<int64_t> miv = Nan::To<int64_t>(lv);\n" <<
          ip << "  if (miv.IsJust()) {\n" <<
          ip << "    " << propName << assign_prefix << "static_cast<" << temp_type << ">(miv.FromJust())" << assign_suffix << ";\n" <<
          ip << "  }\n" <<
          ip << "}\n";
      } else if (pt == AST_PredefinedType::PT_float
              || pt == AST_PredefinedType::PT_double) {
        const std::string temp_type = ltrim(scoped(type->name()));
        strm <<
          ip << "if (lv->IsNumber()) {\n" <<
          ip << "  v8::Local<v8::Number> ln = Nan::To<v8::Number>(lv).ToLocalChecked();\n" <<
          ip << "  " << propName << assign_prefix << "static_cast<" << temp_type << ">(ln->Value())" << assign_suffix << ";\n" <<
          ip << "}\n";
      } else if (pt == AST_PredefinedType::PT_longdouble) {
        strm <<
          ip << "if (lv->IsNumber()) {\n" <<
          ip << "  v8::Local<v8::Number> ln = Nan::To<v8::Number>(lv).ToLocalChecked();\n" <<
          ip << "  ACE_CDR::LongDouble ld;\n" <<
          ip << "  ACE_CDR_LONG_DOUBLE_ASSIGNMENT(ld, ln->Value());\n" <<
          ip << "  " << propName << assign_prefix << "ld" << assign_suffix << ";\n" <<
          ip << "}\n";
      } else if (pt == AST_PredefinedType::PT_boolean) {
        strm <<
          ip << "if (lv->IsBoolean()) {\n" <<
          ip << "  v8::Local<v8::Boolean> lb = Nan::To<v8::Boolean>(lv).ToLocalChecked();\n" <<
          ip << "  " << propName << assign_prefix << "lb->Value()" << assign_suffix << ";\n" <<
          ip << "}\n";
      }

      if (prop_index) {
        strm <<
          "  }\n";
      } else {
        strm <<
          "    }\n"
          "  }\n";
      }

    } else if ((cls & CL_SEQUENCE) && builtInSeq(type)) {
      AST_Type* real_type = resolveActualType(type);
      AST_Sequence* seq = dynamic_cast<AST_Sequence*>(real_type);
      OPENDDS_ASSERT(seq);
      AST_Type* elem = seq->base_type();
      if (prop_index) {
        strm <<
          "  {\n"
          "      v8::Local<v8::Value> lv = Nan::Get(" << src << ", " << prop << ").ToLocalChecked();\n"
          "      if (lv->IsArray()) {\n"
          "        uint32_t length = 0;\n"
          "        v8::Local<v8::Object> lo = Nan::To<v8::Object>(lv).ToLocalChecked();\n"
          "        length = Nan::To<uint32_t>(Nan::Get(lo, Nan::New(\"length\").ToLocalChecked()).ToLocalChecked()).FromJust();\n"
          "        " << scoped(type->name()) << "& temp = " << propName <<  ";\n"
          "        temp.length(length);\n"
          "        for (uint32_t i = 0; i < length; ++i) {\n"
          "        ";
          gen_copyfrom("temp", "lo", elem, "i", true);
          strm <<
          "        }\n"
          "      }\n"
          "  }\n";
      } else {
        strm <<
          "  {\n"
          "    v8::Local<v8::String> field_str = Nan::New(\"" << prop << "\").ToLocalChecked();\n"
          "    if (Nan::Has(" << src << ", field_str).ToChecked()) {\n"
          "      v8::Local<v8::Value> lv = Nan::Get(" << src << ", field_str).ToLocalChecked();\n"
          "      if (lv->IsArray()) {\n"
          "        uint32_t length = 0;\n"
          "        v8::Local<v8::Object> lo = Nan::To<v8::Object>(lv).ToLocalChecked();\n"
          "        length = Nan::To<uint32_t>(Nan::Get(lo, Nan::New(\"length\").ToLocalChecked()).ToLocalChecked()).FromJust();\n"
          "        " << scoped(type->name()) << "& temp = " << propName <<  ";\n"
          "        temp.length(length);\n"
          "        for (uint32_t i = 0; i < length; ++i) {\n"
          "        ";
          gen_copyfrom("temp", "lo", elem, "i", true);
          strm <<
          "        }\n"
          "      }\n"
          "    }\n"
          "  }\n";
      }
    } else { // struct, sequence, etc.
      if (prop_index) {
        strm <<
          "  {\n"
          "    v8::Local<v8::Value> lv = Nan::Get(" << src << ", " << prop << ").ToLocalChecked();\n"
          "    v8::Local<v8::Object> lo = Nan::To<v8::Object>(lv).ToLocalChecked();\n"
          "    copyFromV8(lo, " << propName << (fun_assign ? "())" : ")") << ";\n"
          "  }\n";
      } else {
        strm <<
          "  {\n"
          "    v8::Local<v8::String> field_str = Nan::New(\"" << prop << "\").ToLocalChecked();\n"
          "    if (Nan::Has(" << src << ", field_str).ToChecked()) {\n"
          "      v8::Local<v8::Value> lv = Nan::Get(" << src << ", field_str).ToLocalChecked();\n"
          "      v8::Local<v8::Object> lo = Nan::To<v8::Object>(lv).ToLocalChecked();\n"
          "      copyFromV8(lo, " << propName << (fun_assign ? "())" : ")") << ";\n"
          "    }\n"
          "  }\n";
      }
    }
  }

  struct gen_field_copyto {
    gen_field_copyto(const char* tgt, const char* src) : tgt_(tgt), src_(src) {}
    const std::string tgt_, src_;
    void operator()(AST_Field* field) const
    {
      const std::string fieldName = field->local_name()->get_string(),
        source = src_ + '.' + fieldName,
        prop = "Nan::New<v8::String>(\"" + fieldName + "\").ToLocalChecked()";
      gen_copyto(tgt_.c_str(), source.c_str(),
                 field->field_type(), prop.c_str());
    }
  };

  struct gen_field_copyfrom {
    gen_field_copyfrom(const char* tgt, const char* src) : tgt_(tgt), src_(src) {}
    const std::string tgt_, src_;
    void operator()(AST_Field* field) const
    {
      const std::string fieldName = field->local_name()->get_string();
      gen_copyfrom(tgt_.c_str(), src_.c_str(),
                 field->field_type(), fieldName.c_str());
    }
  };

  void
  gen_type_support(UTL_ScopedName* name)
  {
    be_global->add_include("dds/DCPS/V8TypeConverter.h",
                           BE_GlobalData::STREAM_CPP);
    be_global->impl_ << be_global->versioning_begin() << "\n";
    ScopedNamespaceGuard cppGuard(name, be_global->impl_);
    const std::string lname = name->last_component()->get_string(),
      tsv8 = lname + "TypeSupportV8Impl";
    be_global->impl_ <<
      "class " << tsv8 << "\n"
      "  : public virtual " << lname << "TypeSupportImpl\n"
      "  , public virtual OpenDDS::DCPS::V8TypeConverter {\n\n"
      "  v8::Local<v8::Object> toV8(const void* source) const\n"
      "  {\n"
      "    return OpenDDS::DCPS::copyToV8(*static_cast<const " << lname
               << "*>(source));\n"
      "  }\n\n"
      "  void* fromV8(const v8::Local<v8::Object>& source) const\n"
      "  {\n"
      "    " << lname << "* result = new " << lname << "();\n"
      "    OpenDDS::DCPS::copyFromV8(source, *result);\n"
      "    return result;\n"
      "  }\n\n"
      "  void deleteFromV8Result(void* val) const\n"
      "  {\n"
      "    " << lname << "* delete_me = static_cast< " << lname << "*>(val);\n"
      "    delete delete_me;\n"
      "  }\n\n"
      "  DDS::InstanceHandle_t register_instance_helper(DDS::DataWriter* dw, const void* data) const\n"
      "  {\n"
      "    " << lname << "DataWriter* dw_t = dynamic_cast<" << lname << "DataWriter*>(dw);\n"
      "    return dw_t ? dw_t->register_instance(*static_cast<const " << lname << "*>(data)) : DDS::HANDLE_NIL;\n"
      "  }\n\n"
      "  DDS::ReturnCode_t write_helper(DDS::DataWriter* dw, const void* data, DDS::InstanceHandle_t inst) const\n"
      "  {\n"
      "    " << lname << "DataWriter* dw_t = dynamic_cast<" << lname << "DataWriter*>(dw);\n"
      "    return dw_t ? dw_t->write(*static_cast<const " << lname << "*>(data), inst) : DDS::RETCODE_BAD_PARAMETER;\n"
      "  }\n\n"
      "  DDS::ReturnCode_t unregister_instance_helper(DDS::DataWriter* dw, const void* data, DDS::InstanceHandle_t inst) const\n"
      "  {\n"
      "    " << lname << "DataWriter* dw_t = dynamic_cast<" << lname << "DataWriter*>(dw);\n"
      "    return dw_t ? dw_t->unregister_instance(*static_cast<const " << lname << "*>(data), inst) : DDS::RETCODE_BAD_PARAMETER;\n"
      "  }\n\n"
      "  DDS::ReturnCode_t dispose_helper(DDS::DataWriter* dw, const void* data, DDS::InstanceHandle_t inst) const\n"
      "  {\n"
      "    " << lname << "DataWriter* dw_t = dynamic_cast<" << lname << "DataWriter*>(dw);\n"
      "    return dw_t ? dw_t->dispose(*static_cast<const " << lname << "*>(data), inst) : DDS::RETCODE_BAD_PARAMETER;\n"
      "  }\n\n"
      "public:\n"
      "  struct Initializer {\n"
      "    Initializer()\n"
      "    {\n"
      "      " << lname << "TypeSupport_var ts = new " << tsv8 << ";\n"
      "      ts->register_type(0, \"\");\n"
      "    }\n"
      "  };\n"
      "};\n\n" <<
      tsv8 << "::Initializer init_tsv8_" << lname << ";\n";
    be_global->impl_ << be_global->versioning_end() << "\n";
  }
}

bool v8_generator::gen_struct(AST_Structure* node, UTL_ScopedName* name,
                              const std::vector<AST_Field*>& fields,
                              AST_Type::SIZE_TYPE, const char*)
{
  gen_includes();
  {
    NamespaceGuard ng;
    const std::string clazz = scoped(name);
    {
      Function ctv("copyToV8", "v8::Local<v8::Object>");
      ctv.addArg("src", "const " + clazz + '&');
      ctv.endArgs();
      be_global->impl_ <<
        "  const v8::Local<v8::Object> stru = Nan::New<v8::Object>();\n";
      std::for_each(fields.begin(), fields.end(),
                    gen_field_copyto("stru", "src"));
      be_global->impl_ <<
        "  return stru;\n";
    }
    {
      Function vtc("copyFromV8", "void");
      vtc.addArg("src", "const v8::Local<v8::Object>&");
      vtc.addArg("out", clazz + '&');
      vtc.endArgs();
      std::for_each(fields.begin(), fields.end(),
                    gen_field_copyfrom("out", "src"));
    }
  }

  if (be_global->is_topic_type(node) || idl_global->is_dcps_type(name)) {
    gen_type_support(name);
  }
  return true;
}

bool v8_generator::gen_typedef(AST_Typedef*, UTL_ScopedName* name,
                               AST_Type* type, const char* /*repoid*/)
{
  gen_includes();
  switch (type->node_type()) {
  case AST_Decl::NT_sequence: {
    NamespaceGuard ng;
    AST_Sequence* seq = dynamic_cast<AST_Sequence*>(type);
    OPENDDS_ASSERT(seq);
    const std::string cxx = scoped(name);
    AST_Type* elem = seq->base_type();
    {
      Function ctv("copyToV8", "v8::Local<v8::Object>");
      ctv.addArg("src", "const " + cxx + '&');
      ctv.endArgs();
      be_global->impl_ <<
        "  const v8::Local<v8::Array> tgt(Nan::New<v8::Array>(src.length()));\n"
        "  for (CORBA::ULong i = 0; i < src.length(); ++i) {\n"
        "  ";
      gen_copyto("tgt", "src[i]", elem, "i");
      be_global->impl_ <<
        "  }\n"
        "  return tgt;\n";
    }
    {
      Function vtc("copyFromV8", "void");
      vtc.addArg("src", "const v8::Local<v8::Object>&");
      vtc.addArg("out", cxx + '&');
      vtc.endArgs();
      be_global->impl_ <<
        "  CORBA::ULong length = 0;\n"
        "  if (src->IsArray()) {\n"
        "    length = Nan::To<uint32_t>(Nan::Get(src, Nan::New(\"length\").ToLocalChecked()).ToLocalChecked()).FromJust();\n"
        "  }\n"
        "  out.length(length);\n"
        "  for (CORBA::ULong i = 0; i < length; ++i) {\n"
        "  ";
      gen_copyfrom("out", "src", elem, "i", true);
      be_global->impl_ <<
        "  }\n";
    }
    break;
  }
  case AST_Decl::NT_array: {
    NamespaceGuard ng;
    AST_Array* array = dynamic_cast<AST_Array*>(type);
    OPENDDS_ASSERT(array);
    const std::string cxx = scoped(name);
    AST_Type* elem = array->base_type();
    {
      Function ctv("copyToV8", "v8::Local<v8::Object>");
      ctv.addArg("src", "const " + cxx + '&');
      ctv.endArgs();
      be_global->impl_ <<
        "  CORBA::ULong length = sizeof(src) / sizeof(src[0]);\n"
        "  const v8::Local<v8::Array> tgt(Nan::New<v8::Array>(length));\n"
        "  for (CORBA::ULong i = 0; i < length; ++i) {\n"
        "  ";
      gen_copyto("tgt", "src[i]", elem, "i");
      be_global->impl_ <<
        "  }\n"
        "  return tgt;\n";
    }
    {
      Function vtc("copyFromV8", "void");
      vtc.addArg("src", "const v8::Local<v8::Object>&");
      vtc.addArg("out", cxx + '&');
      vtc.endArgs();
      be_global->impl_ <<
        "  CORBA::ULong length = 0;\n"
        "  if (src->IsArray()) {\n"
        "    CORBA::ULong src_length = Nan::To<uint32_t>(Nan::Get(src, Nan::New(\"length\").ToLocalChecked()).ToLocalChecked()).FromJust();\n"
        "    CORBA::ULong out_length = (sizeof(out) / sizeof(out[0]));\n"
        "    length = (src_length <= out_length) ? src_length : out_length;\n"
        "  }\n"
        "  for (CORBA::ULong i = 0; i < length; ++i) {\n"
        "  ";
      gen_copyfrom("out", "src", elem, "i", true);
      be_global->impl_ <<
        "  }\n";
    }
    break;
  }
  default:
    return true;
  }
  return true;
}

namespace {
  std::string branchGenTo(const std::string&, const std::string& name, AST_Type* type,
                          const std::string&, bool, Intro&,
                          const std::string&)
  {
    const std::string source = "src." + name + "()",
      prop = "Nan::New<v8::String>(\"" + name + "\").ToLocalChecked()";
    std::ostringstream strm;
    strm << "  ";
    gen_copyto("uni", source.c_str(), type, prop.c_str(), strm);
    return strm.str();
  }
  std::string branchGenFrom(const std::string&, const std::string& name, AST_Type* type,
                            const std::string&, bool, Intro&,
                            const std::string&)
  {
    std::ostringstream strm;
    const Classification cls = classify(type);
    if (cls & (CL_STRUCTURE | CL_UNION | CL_SEQUENCE | CL_FIXED)) {
      strm << "  out." << name << "(" << scoped(type->name()) << "());\n";
    }
    strm << "  ";
    gen_copyfrom("out", "src", type, name.c_str(), false, true, strm);
    return strm.str();
  }
}

bool v8_generator::gen_union(AST_Union* node, UTL_ScopedName* name,
                             const std::vector<AST_UnionBranch*>& branches,
                             AST_Type* discriminator, const char* /*repoid*/)
{
  gen_includes();
  NamespaceGuard ng;
  const std::string clazz = scoped(name);
  {
    Function ctv("copyToV8", "v8::Local<v8::Object>");
    ctv.addArg("src", "const " + clazz + '&');
    ctv.endArgs();
    be_global->impl_ <<
      "  const v8::Local<v8::Object> uni = Nan::New<v8::Object>();\n";
    gen_copyto("uni", "src._d()", discriminator, "Nan::New<v8::String>(\"_d\").ToLocalChecked()");
    generateSwitchForUnion(node, "src._d()", branchGenTo, branches, discriminator,
                           "", "", clazz.c_str(), false, false);
    be_global->impl_ <<
      "  return uni;\n";
  }
  {
    Function vtc("copyFromV8", "void");
    vtc.addArg("src", "const v8::Local<v8::Object>&");
    vtc.addArg("out", clazz + '&');
    vtc.endArgs();
    gen_copyfrom("out", "src", discriminator, "_d", false, true);
    generateSwitchForUnion(node, "out._d()", branchGenFrom, branches, discriminator, "", "", clazz.c_str(), false, false);
  }
  if (be_global->is_topic_type(node)) {
    gen_type_support(name);
  }
  return true;
}
