// Copyright 2015 The Shaderc Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libshaderc_util/compiler.h"

#include <cstdint>
#include <fstream>

#include "libshaderc_util/format.h"
#include "libshaderc_util/io.h"
#include "libshaderc_util/resources.h"
#include "libshaderc_util/shader_stage.h"
#include "libshaderc_util/string_piece.h"
#include "libshaderc_util/version_profile.h"

#include "SPIRV/disassemble.h"
#include "SPIRV/doc.h"
#include "SPIRV/GLSL450Lib.h"
#include "SPIRV/GlslangToSpv.h"

#include "message.h"

// We must have the following global variable because declared as extern in
// glslang/SPIRV/disassemble.cpp, which we will need for disassembling.
const char* GlslStd450DebugNames[GLSL_STD_450::Count];

namespace {
using shaderc_util::string_piece;

// For use with glslang parsing calls.
const bool kNotForwardCompatible = false;

// Returns true if #line directive sets the line number for the next line in the
// given version and profile.
inline bool LineDirectiveIsForNextLine(int version, EProfile profile) {
  return profile == EEsProfile || version >= 330;
}

// Returns a #line directive whose arguments are line and filename.
inline std::string GetLineDirective(int line, const string_piece& filename) {
  return "#line " + std::to_string(line) + " \"" + filename.str() + "\"\n";
}

}  // anonymous namespace

namespace shaderc_util {

bool Compiler::Compile(
    const string_piece& input_source_string, EShLanguage forced_shader_stage,
    const string_piece& error_tag,
    const std::function<EShLanguage(std::ostream* error_stream,
                                    const string_piece& error_tag)>&
        stage_callback,
    const Includer& includer, std::ostream* output_stream,
    std::ostream* error_stream) {
  GlslInitializer initializer;
  EShLanguage used_shader_stage = forced_shader_stage;
  const std::string macro_definitions =
      shaderc_util::format(predefined_macros_, "#define ", " ", "\n");
  const std::string pound_extension =
      "#extension GL_GOOGLE_include_directive : enable\n";
  const std::string preamble = macro_definitions + pound_extension;

  std::string preprocessed_shader;

  // If it is preprocess_only_, we definitely need to preprocess. Otherwise, if
  // we don't know the stage until now, we need the preprocessed shader to
  // deduce the shader stage.
  if (preprocess_only_ || used_shader_stage == EShLangCount) {
    bool success;
    std::string glslang_errors;
    std::tie(success, preprocessed_shader, glslang_errors) = PreprocessShader(
        error_tag.str(), input_source_string, preamble, includer);

    success &= PrintFilteredErrors(error_tag, warnings_as_errors_,
                                   /* suppress_warnings = */ true,
                                   glslang_errors.c_str(), &total_warnings_,
                                   &total_errors_);
    if (!success) return false;
    // Because of the behavior change of the #line directive, the #line
    // directive introducing each file's content must use the syntax for the
    // specified version. So we need to probe this shader's version and profile.
    int version;
    EProfile profile;
    std::tie(version, profile) = DeduceVersionProfile(preprocessed_shader);
    const bool is_for_next_line = LineDirectiveIsForNextLine(version, profile);

    preprocessed_shader =
        CleanupPreamble(preprocessed_shader, error_tag, pound_extension,
                        includer.num_include_directives(), is_for_next_line);

    if (preprocess_only_) {
      return shaderc_util::WriteFile(output_stream,
                                     string_piece(preprocessed_shader));
    } else if (used_shader_stage == EShLangCount) {
      std::string errors;
      std::tie(used_shader_stage, errors) =
          GetShaderStageFromSourceCode(error_tag, preprocessed_shader);
      if (!errors.empty()) {
        *error_stream << errors;
        return false;
      }
      if (used_shader_stage == EShLangCount) {
        if ((used_shader_stage = stage_callback(error_stream, error_tag)) ==
            EShLangCount) {
          return false;
        }
      }
    }
  }

  glslang::TShader shader(used_shader_stage);
  const char* shader_strings = input_source_string.data();
  const int shader_lengths = input_source_string.size();
  shader.setStringsWithLengths(&shader_strings, &shader_lengths, 1);
  shader.setPreamble(preamble.c_str());

  // TODO(dneto): Generate source-level debug info if requested.
  bool success =
      shader.parse(&shaderc_util::kDefaultTBuiltInResource, default_version_,
                   default_profile_, force_version_profile_,
                   kNotForwardCompatible, EShMsgDefault, includer);

  success &= PrintFilteredErrors(error_tag, warnings_as_errors_,
                                 suppress_warnings_, shader.getInfoLog(),
                                 &total_warnings_, &total_errors_);
  if (!success) return false;

  glslang::TProgram program;
  program.addShader(&shader);
  success = program.link(EShMsgDefault);
  success &= PrintFilteredErrors(error_tag, warnings_as_errors_,
                                 suppress_warnings_, program.getInfoLog(),
                                 &total_warnings_, &total_errors_);
  if (!success) return false;

  std::vector<uint32_t> spirv;
  glslang::GlslangToSpv(*program.getIntermediate(used_shader_stage), spirv);
  if (disassemble_) {
    spv::Parameterize();
    GLSL_STD_450::GetDebugNames(GlslStd450DebugNames);
    std::ostringstream disassembled_spirv;
    spv::Disassemble(disassembled_spirv, spirv);
    return shaderc_util::WriteFile(output_stream, disassembled_spirv.str());
  } else {
    return shaderc_util::WriteFile(
        output_stream,
        string_piece(
            reinterpret_cast<const char*>(spirv.data()),
            reinterpret_cast<const char*>(&spirv.back()) + sizeof(uint32_t)));
  }
}

void Compiler::AddMacroDefinition(const string_piece& macro,
                                  const string_piece& definition) {
  predefined_macros_[macro] = definition;
}

void Compiler::SetForcedVersionProfile(int version, EProfile profile) {
  default_version_ = version;
  default_profile_ = profile;
  force_version_profile_ = true;
}
void Compiler::OutputMessages() {
  shaderc_util::OutputMessages(total_warnings_, total_errors_);
}

void Compiler::SetDisassemblyMode() { disassemble_ = true; }

void Compiler::SetPreprocessingOnlyMode() { preprocess_only_ = true; }

void Compiler::SetWarningsAsErrors() { warnings_as_errors_ = true; }

void Compiler::SetGenerateDebugInfo() { generate_debug_info_ = true; }

void Compiler::SetSuppressWarnings() { suppress_warnings_ = true; }

std::tuple<bool, std::string, std::string> Compiler::PreprocessShader(
    const std::string& error_tag, const string_piece& shader_source,
    const std::string& shader_preamble, const Includer& includer) {
  // The stage does not matter for preprocessing.
  glslang::TShader shader(EShLangVertex);
  const char* shader_strings = shader_source.data();
  const int shader_lengths = shader_source.size();
  const char* string_names = error_tag.c_str();
  shader.setStringsWithLengthsAndNames(&shader_strings, &shader_lengths,
                                       &string_names, 1);
  shader.setPreamble(shader_preamble.c_str());

  std::string preprocessed_shader;
  const bool success = shader.preprocess(
      &shaderc_util::kDefaultTBuiltInResource, default_version_,
      default_profile_, force_version_profile_, kNotForwardCompatible,
      EShMsgOnlyPreprocessor, &preprocessed_shader, includer);

  if (success) {
    return std::make_tuple(true, preprocessed_shader, shader.getInfoLog());
  }
  return std::make_tuple(false, "", shader.getInfoLog());
}

std::string Compiler::CleanupPreamble(const string_piece& preprocessed_shader,
                                      const string_piece& error_tag,
                                      const string_piece& pound_extension,
                                      int num_include_directives,
                                      bool is_for_next_line) {
  // Those #define directives in preamble will become empty lines after
  // preprocessing. We also injected an #extension directive to turn on #include
  // directive support. In the original preprocessing output from glslang, it
  // appears before the user source string. We need to do proper adjustment:
  // * Remove empty lines generated from #define directives in preamble.
  // * If there is no #include directive in the source code, we do not need to
  //   output the injected #extension directive. Otherwise,
  // * If there exists a #version directive in the source code, it should be
  //   placed at the first line. Its original line will be filled with an empty
  //   line as placeholder to maintain the code structure.

  const std::vector<string_piece> lines =
      preprocessed_shader.get_fields('\n', /* keep_delimiter = */ true);

  std::ostringstream output_stream;

  size_t pound_extension_index = lines.size();
  size_t pound_version_index = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i] == pound_extension) {
      pound_extension_index = std::min(i, pound_extension_index);
    } else if (lines[i].starts_with("#version")) {
      // In a preprocessed shader, directives are in a canonical format, so we
      // can confidently compare to '#version' verbatim, without worrying about
      // whitespace.
      pound_version_index = i;
      if (num_include_directives > 0) output_stream << lines[i];
      break;
    }
  }
  // We know that #extension directive exists and appears before #version
  // directive (if any).
  assert(pound_extension_index < lines.size());

  for (size_t i = 0; i < pound_extension_index; ++i) {
    // All empty lines before the #line directive we injected are generated by
    // preprocessing preamble. Do not output them.
    if (lines[i].strip_whitespace().empty()) continue;
    output_stream << lines[i];
  }

  if (num_include_directives > 0) {
    output_stream << pound_extension;
    // Also output a #line directive for the main file.
    output_stream << GetLineDirective(is_for_next_line, error_tag);
  }

  for (size_t i = pound_extension_index + 1; i < lines.size(); ++i) {
    if (i == pound_version_index) {
      if (num_include_directives > 0) {
        output_stream << "\n";
      } else {
        output_stream << lines[i];
      }
    } else {
      output_stream << lines[i];
    }
  }

  return output_stream.str();
}

std::pair<EShLanguage, std::string> Compiler::GetShaderStageFromSourceCode(
    const string_piece& filename, const std::string& preprocessed_shader) {
  const string_piece kPragmaShaderStageDirective = "#pragma shader_stage";
  const string_piece kLineDirective = "#line";

  int version;
  EProfile profile;
  std::tie(version, profile) = DeduceVersionProfile(preprocessed_shader);
  const bool is_for_next_line = LineDirectiveIsForNextLine(version, profile);

  std::vector<string_piece> lines =
      string_piece(preprocessed_shader).get_fields('\n');
  // The logical line number, which starts from 1 and is sensitive to #line
  // directives, and stage value for #pragma shader_stage() directives.
  std::vector<std::pair<size_t, string_piece>> stages;
  // The physical line numbers of the first #pragma shader_stage() line and
  // first non-preprocessing line in the preprocessed shader text.
  size_t first_pragma_shader_stage = lines.size() + 1;
  size_t first_non_pp_line = lines.size() + 1;

  for (size_t i = 0, logical_line_no = 1; i < lines.size(); ++i) {
    const string_piece current_line = lines[i].strip_whitespace();
    if (current_line.starts_with(kPragmaShaderStageDirective)) {
      const string_piece stage_value =
          current_line.substr(kPragmaShaderStageDirective.size()).strip("()");
      stages.emplace_back(logical_line_no, stage_value);
      first_pragma_shader_stage = std::min(first_pragma_shader_stage, i + 1);
    } else if (!current_line.empty() && !current_line.starts_with("#")) {
      first_non_pp_line = std::min(first_non_pp_line, i + 1);
    }

    // Update logical line number for the next line.
    if (current_line.starts_with(kLineDirective)) {
      // Note that for core profile, the meaning of #line changed since version
      // 330. The line number given by #line used to mean the logical line
      // number of the #line line. Now it means the logical line number of the
      // next line after the #line line.
      logical_line_no =
          std::atoi(current_line.substr(kLineDirective.size()).data()) +
          (is_for_next_line ? 0 : 1);
    } else {
      ++logical_line_no;
    }
  }
  if (stages.empty()) return std::make_pair(EShLangCount, "");

  std::string error_message;

  // TODO(antiagainst): #line could change the effective filename once we add
  // support for filenames in #line directives.

  if (first_pragma_shader_stage > first_non_pp_line) {
    error_message += filename.str() + ":" + std::to_string(stages[0].first) +
                     ": error: '#pragma': the first 'shader_stage' #pragma "
                     "must appear before any non-preprocessing code\n";
  }

  EShLanguage stage = MapStageNameToLanguage(stages[0].second);
  if (stage == EShLangCount) {
    error_message +=
        filename.str() + ":" + std::to_string(stages[0].first) +
        ": error: '#pragma': invalid stage for 'shader_stage' #pragma: '" +
        stages[0].second.str() + "'\n";
  }

  for (size_t i = 1; i < stages.size(); ++i) {
    if (stages[i].second != stages[0].second) {
      error_message += filename.str() + ":" + std::to_string(stages[i].first) +
                       ": error: '#pragma': conflicting stages for "
                       "'shader_stage' #pragma: '" +
                       stages[i].second.str() + "' (was '" +
                       stages[0].second.str() + "' at " + filename.str() + ":" +
                       std::to_string(stages[0].first) + ")\n";
    }
  }

  return std::make_pair(error_message.empty() ? stage : EShLangCount,
                        error_message);
}

std::pair<int, EProfile> Compiler::DeduceVersionProfile(
    const std::string& preprocessed_shader) {
  int version = default_version_;
  EProfile profile = default_profile_;
  if (!force_version_profile_) {
    std::tie(version, profile) =
        GetVersionProfileFromSourceCode(preprocessed_shader);
    if (version == 0 && profile == ENoProfile) {
      version = default_version_;
      profile = default_profile_;
    }
  }
  return std::make_pair(version, profile);
}

std::pair<int, EProfile> Compiler::GetVersionProfileFromSourceCode(
    const std::string& preprocessed_shader) {
  string_piece pound_version = preprocessed_shader;
  const size_t pound_version_loc = pound_version.find("#version");
  if (pound_version_loc == string_piece::npos) {
    return std::make_pair(0, ENoProfile);
  }
  pound_version =
      pound_version.substr(pound_version_loc + std::strlen("#version"));
  pound_version = pound_version.substr(0, pound_version.find_first_of("\n"));

  std::string version_profile;
  for (const auto character : pound_version) {
    if (character != ' ') version_profile += character;
  }

  int version;
  EProfile profile;
  if (!ParseVersionProfile(version_profile, &version, &profile)) {
    return std::make_pair(0, ENoProfile);
  }
  return std::make_pair(version, profile);
}

}  // namesapce glslc