# 生成跨平台语言数据：把 resources/strings-*.txt 四份 UTF-8 文案编成一份 C++ 源文件。
#
# 为什么不再靠 Windows 的 RCDATA：那条路只有 rc.exe 能走，共用语言层就得 #include <windows.h>，
# Linux 上根本没有这份资源。改成"构建期生成一份 C++ 源文件"以后，两个平台编译的是同一份嵌入物，
# 而 CMake 的自定义命令带着 DEPENDS，改完文案必然重新生成、必然重新编译 —— 这条正是
# resources/ecapture.rc 里当年用 OBJECT_DEPENDS 才勉强办到的事。
#
# 用法（由 CMakeLists.txt 的自定义命令调用，人不直接跑）：
#   cmake -DECP_RESOURCES_DIR=<resources 目录> -DECP_OUTPUT=<生成的 .cpp 路径> -P scripts/gen-lang-data.cmake
#
# 规矩：
#   * 原文按字节嵌进原始字符串字面量，不转义、不改写：文案里有大量 Windows 路径与成对引号，
#     任何转义层都是第二次出错的机会。原始字符串的闭合序列 )DELIM" 在生成前查一遍，撞上就报错。
#   * NUL 字节一律拒绝（.rc 那一路按字节嵌无所谓，C++ 字面量里它会截断内容）。
#   * CMake 的 file(READ) 把行尾的 \r\n 归一成 \n；Lang.cpp 的表解析本来就忽略 \r，
#     所以这一点不影响任何一句文案，只是不再逐字节相同 —— 四份源文件保持 LF 就好。

if(NOT DEFINED ECP_RESOURCES_DIR OR NOT DEFINED ECP_OUTPUT)
  message(FATAL_ERROR "gen-lang-data.cmake 需要 -DECP_RESOURCES_DIR= 与 -DECP_OUTPUT=")
endif()

set(ECP_DELIM "ECAP_LANG_DATA")

# 嵌入名与源文件的对应关系。名字沿用当年 .rc 里那四个，出问题时两边能对上号。
set(ECP_NAMES ECP_TEXT_ZH_CN;ECP_TEXT_ZH_TW;ECP_TEXT_EN;ECP_TEXT_JA)
set(ECP_FILES strings-zh-CN.txt;strings-zh-TW.txt;strings-en.txt;strings-ja.txt)
set(ECP_VARS  kZhCn;kZhTw;kEn;kJa)

list(LENGTH ECP_NAMES _ec_count)
list(LENGTH ECP_FILES _ec_file_count)
list(LENGTH ECP_VARS _ec_var_count)
if(NOT _ec_count EQUAL _ec_file_count OR NOT _ec_count EQUAL _ec_var_count)
  message(FATAL_ERROR "内建表长度不一致（改脚本请三张表一起改）")
endif()

math(EXPR _ec_last "${_ec_count} - 1")
set(ECP_BLOB_DEFS "")
set(ECP_BLOB_ROWS "")

foreach(_ec_i RANGE ${_ec_last})
  list(GET ECP_NAMES ${_ec_i} _ec_name)
  list(GET ECP_FILES ${_ec_i} _ec_file)
  list(GET ECP_VARS ${_ec_i} _ec_var)
  set(_ec_path "${ECP_RESOURCES_DIR}/${_ec_file}")

  if(NOT EXISTS "${_ec_path}")
    message(FATAL_ERROR "缺少文案文件：${_ec_path}")
  endif()

  # HEX 这一份只用来查 NUL 与报字节数：它不经文本处理，是文件真实大小。
  file(READ "${_ec_path}" _ec_hex HEX)
  string(LENGTH "${_ec_hex}" _ec_hex_len)
  math(EXPR _ec_bytes "${_ec_hex_len} / 2")
  if(_ec_bytes LESS_EQUAL 0)
    message(FATAL_ERROR "${_ec_file} 是空文件：整份语言会退成 ?key")
  endif()
  string(REGEX MATCHALL ".." _ec_pairs "${_ec_hex}")
  string(FIND ";${_ec_pairs};" ";00;" _ec_nul)
  if(NOT _ec_nul EQUAL -1)
    message(FATAL_ERROR "${_ec_file} 里有 NUL 字节：C++ 字面量装不下（它会在运行期截断整份文案），请先清掉")
  endif()

  file(READ "${_ec_path}" _ec_text)
  string(FIND "${_ec_text}" ")${ECP_DELIM}\"" _ec_close)
  if(NOT _ec_close EQUAL -1)
    message(FATAL_ERROR "${_ec_file} 里出现了原始字符串的闭合序列 )${ECP_DELIM}\"，换分隔符")
  endif()

  set(ECP_BLOB_DEFS "${ECP_BLOB_DEFS}static const char ${_ec_var}[] = R\"${ECP_DELIM}(${_ec_text})${ECP_DELIM}\";\n")
  set(ECP_BLOB_ROWS "${ECP_BLOB_ROWS}    {\"${_ec_name}\", {${_ec_var}, sizeof(${_ec_var}) - 1u}},\n")
  message(STATUS "嵌入文案 ${_ec_name} <= ${_ec_file}（${_ec_bytes} 字节）")
endforeach()

# 目标文件内容没变时不重写：否则每次构建都碰到时间戳，下游全部重编。
set(ECP_GENERATED
"#define ECAPTURE_LANG_DATA_GENERATED 1
// 本文件由 scripts/gen-lang-data.cmake 从 resources/strings-*.txt 生成，请勿手工编辑。
// 四份文案的 UTF-8 字节按原样嵌在下面，运行期由 src/Lang.cpp 解析成 key -> 文案 的表。
#include \"LangData.h\"

#include <cstddef>
#include <string_view>

namespace ecapture {
namespace {

${ECP_BLOB_DEFS}
struct NamedBlob {
    const char* name;
    LangBlob blob;
};

const NamedBlob kLangBlobs[] = {
${ECP_BLOB_ROWS}};

}  // namespace

const LangBlob* FindLangBlob(const char* name) {
    if (!name) return nullptr;
    const std::string_view wanted(name);
    for (const NamedBlob& e : kLangBlobs) {
        if (wanted == e.name) return &e.blob;
    }
    return nullptr;
}

}  // namespace ecapture
")

set(ECP_GENERATED_CURRENT "")
if(EXISTS "${ECP_OUTPUT}")
  file(READ "${ECP_OUTPUT}" ECP_GENERATED_CURRENT)
endif()
if(NOT ECP_GENERATED_CURRENT STREQUAL ECP_GENERATED)
  get_filename_component(ECP_OUTPUT_DIR "${ECP_OUTPUT}" DIRECTORY)
  file(MAKE_DIRECTORY "${ECP_OUTPUT_DIR}")
  file(WRITE "${ECP_OUTPUT}" "${ECP_GENERATED}")
  message(STATUS "已生成语言数据源文件：${ECP_OUTPUT}")
else()
  message(STATUS "语言数据源文件已是最新：${ECP_OUTPUT}")
endif()
