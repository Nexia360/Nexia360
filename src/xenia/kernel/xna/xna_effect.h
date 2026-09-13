/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNA_XNA_EFFECT_H_
#define XENIA_KERNEL_XNA_XNA_EFFECT_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xenia/gpu/shader.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace kernel {
namespace xna {

// One shader recovered from an effect container, already analyzed by the same
// class the GPU uses for microcode found in guest memory.
// A candidate shader located in an effect container: where it is and how far
// it runs. It is NOT analyzed at this point - see FindEffectShaders for why -
// so `shader` stays null until a pass names this one and says which kind it is.
// One blank vfetch_full in a vertex shader, and the vertex element it is
// waiting to be told about. The console's D3D9 runtime fills the fetch's
// stride, offset and format in from the vertex declaration at draw time - the
// compiled instruction carries none of them - so this is what has to be matched
// against the declaration before the shader can fetch anything.
struct EffectFetchSemantic {
  // Index of the instruction in the microcode: it lives at ucode dword
  // instruction * 3.
  uint32_t instruction = 0;
  // D3DDECLUSAGE, which is NOT the XNA VertexElementUsage numbering.
  uint32_t usage = 0;
  uint32_t usage_index = 0;
};

struct EffectShaderInfo {
  gpu::xenos::ShaderType type = gpu::xenos::ShaderType::kVertex;
  uint32_t dword_offset = 0;
  uint32_t dword_count = 0;
  bool found = false;
  // The parameters THIS shader uses, in the order its own constant table lists
  // them - which D3DX writes sorted by name. Constant registers are assigned
  // per shader over only these, starting near zero; a running total across
  // every parameter in the effect produces registers past the 256 the hardware
  // has. Empty when the table could not be read, in which case nothing is
  // mapped rather than something being mapped wrongly.
  // The float literals compiled into the shader, which sit between its
  // descriptor and its microcode. They are not parameters - the compiler put
  // them at the top of the constant file and the code addresses them as c255
  // downwards - so nothing the title sets will ever fill them, and a shader
  // multiplying by an unwritten c255 produces nothing.
  std::vector<float> literals;

  // The microcode as the effect stores it, before any vertex declaration has
  // been applied to it. The Shader below was built from this and is only good
  // for reading; a draw needs a copy with its fetches filled in.
  std::vector<uint32_t> ucode;
  std::vector<EffectFetchSemantic> fetch_semantics;

  std::vector<std::string> constant_names;
  // Parameter name to its first constant register, for THIS shader. Filled only
  // when the registers the names imply are exactly the registers the microcode
  // was found to read - two independent sources agreeing. Left empty otherwise,
  // so a mapping that cannot be trusted is never used.
  std::map<std::string, uint32_t> constant_registers;
  // How many registers each name occupies, as its own constant table states it.
  // The compiler drops rows it never reads - _WorldToObject is declared 4x4 and
  // allocated three registers - so writing four over a three register parameter
  // spills into whatever the compiler put next.
  std::map<std::string, uint32_t> constant_counts;
  std::map<std::string, std::vector<float>> constant_defaults;
  // The sampler register each sampler name was given, which is a different
  // register file from the float constants and shares nothing with it.
  std::map<std::string, uint32_t> sampler_registers;
  // SHARED, not owned outright, so an EffectImage can be copied - which is
  // what Effect.Clone() does. A clone carries byte-identical microcode, so
  // the translated shader is the same shader; copying it would mean
  // translating it a second time to get an identical result, and every
  // BasicEffect loaded from content arrives as a clone.
  std::shared_ptr<gpu::Shader> shader;
};

// One pass of a technique, and which shader objects it binds. These come from
// the effect tables rather than from the byte scan, which is what makes them
// trustworthy: a pass says outright that a shader is a vertex or a pixel one.
// A parameter as the effect declares it. The NAME is the point: a title looks
// its parameters up by name and dereferences the result immediately, so a
// nameless table is a null reference at the first property set.
struct EffectParameterInfo {
  std::string name;
  uint32_t type = 0;             // D3DXPARAMETER_TYPE
  uint32_t parameter_class = 0;  // D3DXPARAMETER_CLASS
  uint32_t rows = 0;
  uint32_t columns = 0;
  uint32_t elements = 0;
  uint32_t value_offset = 0;
};

// A shader object inside the effect: its id, and where its microcode is.
struct EffectObjectInfo {
  uint32_t id = 0;
  uint32_t byte_offset = 0;
  uint32_t byte_size = 0;
  uint32_t technique = UINT32_MAX;
  uint32_t pass = UINT32_MAX;
  uint32_t state = UINT32_MAX;
};

struct EffectShaderSelector {
  bool valid = false;
  std::string array_name;
  uint32_t preshader_offset = 0;
  uint32_t preshader_size = 0;
  std::vector<uint32_t> element_object_indices;
};

struct EffectNamedShader {
  std::string name;
  const uint8_t* pointer = nullptr;
  uint32_t guest_address = 0;
  uint32_t object_id = 0;
  uint32_t object_index = UINT32_MAX;
  uint32_t element = 0;
};

struct EffectPassInfo {
  std::string name;
  bool has_vertex_shader = false;
  bool has_pixel_shader = false;
  // The OBJECT IDS the pass binds, read from the record its state points at -
  // not a position, and not a nearest-match.
  uint32_t vertex_object_id = 0;
  uint32_t pixel_object_id = 0;
  // Indices into EffectImage::shaders, filled the first time this pass is
  // applied - not at load. Analyzing every object an effect contains means one
  // malformed shader takes down a title that would never have drawn with it;
  // an effect here holds a hundred and a pass uses two.
  uint32_t vertex_shader_index = UINT32_MAX;
  uint32_t pixel_shader_index = UINT32_MAX;
  // Indices into EffectImage::objects, decided at load, analyzed on use.
  uint32_t vertex_object_index = UINT32_MAX;
  uint32_t pixel_object_index = UINT32_MAX;
  bool shaders_resolved = false;
  uint32_t vertex_state_index = UINT32_MAX;
  uint32_t pixel_state_index = UINT32_MAX;
  EffectShaderSelector vertex_selector;
  EffectShaderSelector pixel_selector;
};

struct EffectTechniqueInfo {
  std::string name;
  std::vector<EffectPassInfo> passes;
};

struct EffectImage {
  bool valid = false;
  bool was_byte_swapped = false;
  // True when the container was found already in host order, meaning an earlier
  // load of the same array swapped it. The strings still need un-swapping.
  bool was_already_swapped = false;
  uint32_t size = 0;
  // Read from the D3DX header inside the container. These are what EFFECT_DESC
  // reports, and the technique count is what the constructor indexes into
  // immediately, so a zero here is a null CurrentTechnique.
  uint32_t parameter_count = 0;
  uint32_t technique_count = 0;
  uint32_t object_count = 0;
  uint32_t header_offset = 0;
  std::vector<EffectShaderInfo> shaders;
  // Empty when the tables could not be walked, in which case only the scanned
  // candidates above are known.
  std::vector<EffectParameterInfo> parameters;
  std::vector<EffectObjectInfo> objects;
  std::vector<EffectNamedShader> vertex_shader_table;
  std::vector<EffectNamedShader> pixel_shader_table;
  // Where the object section begins, recorded by the technique walk.
  uint32_t objects_offset = 0;
  // The effect body, kept so a pass can be analyzed when it is first applied
  // rather than at load.
  std::shared_ptr<std::vector<uint8_t>> owned_body;
  const uint8_t* body = nullptr;
  uint32_t body_size = 0;
  std::vector<EffectTechniqueInfo> techniques;
  uint32_t current_technique = 0;
  std::map<uint64_t, uint32_t> analyzed_objects;
};

void XnaSetEffectTechnique(uint32_t handle, uint32_t technique);

// Validates a compiled console effect, puts it into host byte order in place -
// the way a XEX image is checked and swapped before anything inside it is read
// - and locates the shaders it contains.
bool PrepareEffectImage(uint8_t* data, uint32_t size, EffectImage* image);

// Locates every Xenos shader in an already-swapped container by decoding
// control-flow programs, rather than by trusting a layout.
void FindEffectShaders(const uint8_t* data, uint32_t size, EffectImage* image);

uint32_t RegisterEffect(std::unique_ptr<EffectImage> image);

// The handle assigned to a blob already prepared by the managed hook, so
// D3D_Effect_CreateHandle can pick up the image that was built when the bytes
// first arrived rather than parsing them a second time. Zero if unseen.
uint32_t FindPreparedEffect(const uint8_t* data, uint32_t size);
uint64_t ContentHashForRegistration(const uint8_t* data, uint32_t size);
void NoteEffectContent(uint64_t hash, uint32_t handle);
void NotePreparedOnThisThread(uint32_t handle);

// Reads the D3DX fx_2_0 header of the effect body - the blob the container
// points at, and the one CreateEffect is handed.
bool ParseD3dxEffect(const uint8_t* data, uint32_t size, EffectImage* image);

// Walks the technique, pass and state tables, recording which shader each pass
// binds and whether it is the vertex or the pixel one. Returns false, having
// changed nothing, if the tables cannot be believed whole.
bool WalkEffectTables(const uint8_t* data, uint32_t size, EffectImage* image);
uint32_t TotalPasses(const EffectImage& image);

// Reads the object records that follow the techniques and records every shader
// blob among them.
void ParseEffectObjects(const uint8_t* body, uint32_t size, EffectImage* image);

void BuildEffectShaderTables(const uint8_t* body, uint32_t size,
                             EffectImage* image);
const EffectNamedShader* EffectShaderAt(const EffectImage& image,
                                        gpu::xenos::ShaderType stage,
                                        uint32_t index);

// Ties each pass to the microcode it binds, by object id, and analyzes only
// those shaders.
void ResolvePassShaders(const uint8_t* body, uint32_t size, EffectImage* image);

uint32_t AnalyzeObject(EffectImage* image, const EffectObjectInfo& object,
                       gpu::xenos::ShaderType type, const uint8_t* body);

// Analyzes a located shader, now that a pass has vouched for it and said which
// kind it is. Nothing is analyzed before a pass names it.

EffectImage* FindEffect(uint32_t handle);

// Analyzes the two objects a pass binds, the first time that pass is applied.
// An effect holds a hundred shader objects and a pass uses two of them, so
// doing this at load meant one malformed object could end a title that would
// never have drawn with it. Safe to call repeatedly - it does its work once.
using EffectParameterLookup =
    std::function<bool(const std::string& name, uint32_t reg, float* out)>;
void ResolvePassShadersOnUse(
    EffectImage* image, EffectPassInfo* pass,
    const EffectParameterLookup& lookup = EffectParameterLookup());
bool EvaluatePreshader(const EffectImage& image,
                       const EffectShaderSelector& selector,
                       const EffectParameterLookup& lookup, float* out);

}  // namespace xna
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNA_XNA_EFFECT_H_
