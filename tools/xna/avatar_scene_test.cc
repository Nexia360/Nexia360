#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "xenia/kernel/xna/xna_avatar_format.h"

extern "C" void xenia_log(const char*, ...) {}

using namespace xe::kernel::xna::avatar;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf(
        "usage: avatar_scene_test <AvatarAssetPack.toc> <out.png> [seed] "
        "[clip] [seconds] [yaw]\n");
    return 1;
  }
  Catalog catalog;
  if (!catalog.Load(argv[1])) {
    std::printf("cannot load %s\n", argv[1]);
    return 1;
  }
  if (std::string(argv[2]) == "carryable" && argc > 4) {
    FILE* in = std::fopen(argv[3], "rb");
    if (!in) {
      return 1;
    }
    std::vector<uint8_t> blob;
    uint8_t chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
      blob.insert(blob.end(), chunk, chunk + got);
    }
    std::fclose(in);
    std::array<uint8_t, 16> id = {};
    const std::string hex = argv[4];
    for (size_t i = 0; i < id.size() && i * 2 + 1 < hex.size(); ++i) {
      id[i] = uint8_t(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
    }
    catalog.AddAsset(id, "external", blob);
    const Entry* entry = catalog.FindAsset(id.data());
    std::vector<Record> records;
    catalog.Records(entry->index, &records);
    for (const Record& record : records) {
      if (record.type == 5) {
        for (size_t offset = 0;
             offset + 4 < record.data.size() && offset <= 0x30; offset += 4) {
          Skeleton skeleton;
          if (DecodeSkeleton(record.data.data() + offset,
                             record.data.size() - offset, &skeleton)) {
            std::printf("t5 skeleton at +%zX: %u joints\n", offset,
                        skeleton.count);
            for (uint32_t j = 0; j < skeleton.count; ++j) {
              std::printf("  joint %2u parent %3u bind %8.4f %8.4f %8.4f\n", j,
                          skeleton.parents[j], skeleton.bind[j][0],
                          skeleton.bind[j][1], skeleton.bind[j][2]);
            }
          }
        }
      }
      if (record.type == 1) {
        Clip body;
        Clip carried;
        const bool a = DecodeClip(record.data, &body);
        const bool b = DecodeCarryableClip(record.data, &carried);
        std::printf(
            "t1: body %s (%u frames, %u joints), carryable %s (%u "
            "frames, %u joints)\n",
            a ? "ok" : "failed", body.frames, body.joints, b ? "ok" : "failed",
            carried.frames, carried.joints);
      }
    }
    return 0;
  }
  if (std::string(argv[2]) == "asset" && argc > 4) {
    FILE* in = std::fopen(argv[3], "rb");
    if (!in) {
      return 1;
    }
    std::vector<uint8_t> blob;
    uint8_t chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
      blob.insert(blob.end(), chunk, chunk + got);
    }
    std::fclose(in);
    std::array<uint8_t, 16> id = {};
    const std::string hex = argv[4];
    for (size_t i = 0; i < id.size() && i * 2 + 1 < hex.size(); ++i) {
      id[i] = uint8_t(std::strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
    }
    if (!catalog.AddAsset(id, "external", blob)) {
      std::printf("AddAsset refused\n");
      return 1;
    }
    const Entry* entry = catalog.FindAsset(id.data());
    std::printf("asset entry %04X kind %08X body %u slot %d\n", entry->index,
                entry->kind, entry->BodyMask(), PrimarySlot(entry->kind));
    std::vector<Record> records;
    catalog.Records(entry->index, &records);
    for (const Record& record : records) {
      std::printf("  record type %u, %zu bytes\n", record.type,
                  record.data.size());
    }
    auto model = catalog.LoadModel(entry->index);
    if (!model) {
      std::printf("model did not decode\n");
      return 1;
    }
    size_t vertices = 0;
    for (const Batch& batch : model->batches) {
      vertices += batch.vertices.size();
    }
    std::printf("model: %zu batches, %zu vertices, %zu textures\n",
                model->batches.size(), vertices, model->textures.size());
    if (argc > 5) {
      Description description;
      description.items[kSlotCarryable] = uint16_t(entry->index);
      const Scene scene = BuildScene(catalog, description);
      Matrix local[kMaxJoints];
      BindPose(scene.skeleton, local);
      Matrix carried[kMaxJoints];
      const Matrix* carried_pose = nullptr;
      if (scene.carryable) {
        const float seconds = argc > 6 ? float(std::atof(argv[6])) : 0.0f;
        if (scene.carryable->body) {
          SamplePose(*scene.carryable->body, scene.skeleton, seconds, local);
        }
        SampleCarryable(*scene.carryable, seconds, carried);
        carried_pose = carried;
      }
      std::vector<uint8_t> rgba;
      RenderPreview(scene, local, carried_pose, Expression(), 512, 512, 0.6f,
                    &rgba);
      const std::vector<uint8_t> png = EncodePng(512, 512, rgba);
      FILE* out = std::fopen(argv[5], "wb");
      if (out) {
        std::fwrite(png.data(), 1, png.size(), out);
        std::fclose(out);
      }
    }
    return 0;
  }
  if (std::string(argv[2]) == "manifest" && argc > 3) {
    FILE* in = std::fopen(argv[3], "rb");
    if (!in) {
      return 1;
    }
    std::vector<uint8_t> original(kManifestBytes);
    const size_t got = std::fread(original.data(), 1, original.size(), in);
    std::fclose(in);
    Description description;
    if (got != original.size() ||
        !ParseManifest(&catalog, original.data(), original.size(),
                       &description)) {
      std::printf("not a manifest\n");
      return 1;
    }
    std::printf("body %u height %u weight %u components %zu\n",
                description.body, description.height, description.weight,
                description.components.size());
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
      const uint16_t item = description.items[slot];
      const Entry* entry = item == kNoItem ? nullptr : catalog.Find(item);
      std::printf("  %-12s %04X %s\n", SlotName(slot), item,
                  entry ? entry->name.c_str() : "-");
    }
    for (uint32_t color = 0; color < kColorCount; ++color) {
      std::printf("  colour %-12s %08X\n", ColorName(color),
                  description.colors[color]);
    }
    uint64_t xuid = 0;
    for (int i = 0; i < 8; ++i) {
      xuid = (xuid << 8) | original[0x380 + i];
    }
    const auto rebuilt = SerializeManifest(&catalog, description, xuid);
    size_t differ = 0;
    for (size_t i = 0; i < rebuilt.size(); ++i) {
      if (rebuilt[i] != original[i]) {
        if (differ < 64) {
          std::printf("  differs at %03zX: %02X -> %02X\n", i, original[i],
                      rebuilt[i]);
        }
        ++differ;
      }
    }
    std::printf("rebuilt differs in %zu bytes\n", differ);
    if (argc > 4) {
      FILE* out = std::fopen(argv[4], "wb");
      if (out) {
        std::fwrite(rebuilt.data(), 1, rebuilt.size(), out);
        std::fclose(out);
      }
    }
    return 0;
  }
  if (std::string(argv[2]) == "entries") {
    uint32_t highest = 0;
    for (const Entry& entry : catalog.entries()) {
      highest = entry.index > highest ? entry.index : highest;
    }
    std::printf("%zu entries, highest index %04X\n", catalog.entries().size(),
                highest);
    for (int i = 3; i < argc; ++i) {
      const uint32_t index = uint32_t(std::strtoul(argv[i], nullptr, 16));
      const Entry* entry = catalog.Find(index);
      std::string types;
      std::vector<Record> records;
      if (entry && catalog.Records(index, &records)) {
        for (const Record& record : records) {
          types += std::to_string(record.type) + " ";
        }
      }
      std::printf("%04X %-7s kind %08X records [%s] %s\n", index,
                  entry ? "found" : "missing", entry ? entry->kind : 0,
                  types.c_str(), entry ? entry->name.c_str() : "-");
      if (entry) {
        std::printf("     own asset id ");
        for (uint8_t byte : entry->asset_id) {
          std::printf("%02X", byte);
        }
        std::printf("\n");
      }
      for (const Entry& other : catalog.entries()) {
        const uint8_t* id = other.asset_id.data();
        if (id[0] == 0x00 && id[1] == 0x40 && id[2] == 0x00 && id[3] == 0x00 &&
            ((uint32_t(id[4]) << 8) | id[5]) == index) {
          std::printf("     asset id of %04X kind %08X '%s': ", other.index,
                      other.kind, other.name.c_str());
          for (uint8_t byte : other.asset_id) {
            std::printf("%02X", byte);
          }
          std::printf("\n");
        }
      }
    }
    return 0;
  }
  const uint32_t seed = argc > 3 ? uint32_t(std::atoi(argv[3])) : 1;
  const int32_t clip_index = argc > 4 ? std::atoi(argv[4]) : -1;
  const float seconds = argc > 5 ? float(std::atof(argv[5])) : 0.0f;
  const float yaw = argc > 6 ? float(std::atof(argv[6])) : 0.0f;
  std::mt19937 rng(seed);
  const Description description = RandomDescription(catalog, rng, -1);
  std::printf("%zu entries, body %u height %u\n", catalog.entries().size(),
              description.body, description.height);
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    const uint16_t item = description.items[slot];
    const Entry* entry = item == kNoItem ? nullptr : catalog.Find(item);
    std::printf("  %-11s %5d %08X %s\n", SlotName(slot),
                item == kNoItem ? -1 : int(item), entry ? entry->kind : 0,
                entry ? entry->name.c_str() : "-");
  }
  const auto bytes = SerializeDescription(description);
  Description parsed;
  const bool round_trip =
      ParseDescription(bytes.data(), bytes.size(), &parsed) &&
      parsed.items == description.items &&
      parsed.colors == description.colors &&
      parsed.custom == description.custom && parsed.body == description.body;
  std::printf("description round trip %s\n", round_trip ? "ok" : "FAILED");
  {
    const Skeleton& skeleton = MainSkeleton();
    std::printf("skeleton parents (%u):", skeleton.count);
    for (uint32_t j = 0; j < skeleton.count; ++j) {
      std::printf("%s%d", j ? "," : " ",
                  skeleton.parents[j] < j ? int(skeleton.parents[j]) : -1);
    }
    std::printf("\n");
    for (uint32_t j = 0; j < skeleton.count; ++j) {
      const uint32_t parent = skeleton.parents[j];
      std::printf("  bone %2u parent %2d bind %7.3f %7.3f %7.3f\n", j,
                  parent < j ? int(parent) : -1, skeleton.bind[j][0],
                  skeleton.bind[j][1], skeleton.bind[j][2]);
    }
  }

  for (const Component& component : Components(catalog, description)) {
    auto raw = catalog.LoadRawModel(component.entry);
    if (!raw) {
      std::printf("raw %u FAILED\n", component.entry);
      continue;
    }
    size_t texture_bytes = 0;
    bool fits = true;
    for (const RawTexture& texture : raw->textures) {
      texture_bytes += texture.data.size();
      fits = fits &&
             uint64_t(texture.gpu_offset) + texture.gpu_size <= raw->gpu_size;
    }
    for (const RawBatch& batch : raw->batches) {
      fits = fits &&
             uint64_t(batch.vb_offset) +
                     uint64_t(batch.stride) * batch.vertices.size() <=
                 raw->gpu_size &&
             uint64_t(batch.ib_offset) + batch.indices.size() * 2 <=
                 raw->gpu_size &&
             batch.stride == 28 + 4 * batch.uv_sets;
    }
    std::printf(
        "raw %u mask %04X: cpu %X gpu %X, %zu batches, %zu textures (%zu "
        "stored bytes), layout %s\n",
        component.entry, component.mask, raw->cpu_size, raw->gpu_size,
        raw->batches.size(), raw->textures.size(), texture_bytes,
        fits ? "fits" : "DOES NOT FIT");
    for (size_t t = 0; t < raw->textures.size(); ++t) {
      const RawTexture& texture = raw->textures[t];
      uint32_t nonzero = 0;
      uint32_t alpha_zero_blocks = 0;
      for (uint8_t byte : texture.data) {
        nonzero += byte != 0;
      }
      if ((texture.format & 0x3F) == 0x14) {
        for (size_t at = 0; at + 16 <= texture.data.size(); at += 16) {
          alpha_zero_blocks +=
              texture.data[at] == 0 && texture.data[at + 1] == 0;
        }
      }
      std::printf(
          "  tex %zu: fmt %08X %ux%u x%u pitch %u rows %u slice %X total %X "
          "gpu %X+%X zero_fill %d tiled %d data %zu nonzero %u a0blocks %u\n",
          t, texture.format, texture.width, texture.height, texture.slices,
          texture.pitch, texture.rows, texture.slice_size, texture.total_size,
          texture.gpu_offset, texture.gpu_size, texture.zero_fill,
          texture.tiled, texture.data.size(), nonzero, alpha_zero_blocks);
    }
    for (const RawBatch& batch : raw->batches) {
      std::printf("  shader %u:", batch.shader);
      for (const Param& param : batch.params) {
        std::printf(" [t%u u%u %08X %08X %08X %08X]", param.type, param.usage,
                    param.data[0], param.data[1], param.data[2], param.data[3]);
      }
      std::printf("\n");
    }
  }
  for (uint32_t feature = 0; feature < kFeatureCount; ++feature) {
    const int32_t entry = FeatureEntry(description, feature);
    if (entry < 0) {
      continue;
    }
    auto raw = catalog.LoadRawTexture(uint32_t(entry));
    std::printf("raw feature %u: entry %d %s %ux%u x%u slice %X total %X\n",
                feature, entry, raw ? "ok" : "FAILED", raw ? raw->width : 0,
                raw ? raw->height : 0, raw ? raw->slices : 0,
                raw ? raw->slice_size : 0, raw ? raw->total_size : 0);
  }
  uint32_t linear_textures = 0;
  uint32_t linear_fit = 0;
  for (const Entry& entry : catalog.entries()) {
    auto raw = catalog.LoadRawModel(entry.index);
    if (!raw) {
      continue;
    }
    for (const RawTexture& texture : raw->textures) {
      const uint32_t base = texture.format & 0x3F;
      if (texture.tiled || texture.zero_fill ||
          (base != 0x12 && base != 0x13 && base != 0x14 && base != 0x06)) {
        continue;
      }
      const uint32_t block_pixels = base == 0x06 ? 1 : 4;
      const uint32_t block_bytes = base == 0x12 ? 8 : base == 0x06 ? 4 : 16;
      const uint32_t blocks_wide =
          (texture.width + block_pixels - 1) / block_pixels;
      const uint32_t padded = ((blocks_wide + 31) / 32) * 32 * block_bytes;
      ++linear_textures;
      const bool fits = texture.pitch == blocks_wide * block_bytes &&
                        uint64_t(padded) * texture.rows <= texture.slice_size;
      linear_fit += fits;
      if (!fits) {
        std::printf(
            "linear %u %s: fmt %08X %ux%u pitch %u padded %u rows %u slice "
            "%X\n",
            entry.index, entry.name.c_str(), texture.format, texture.width,
            texture.height, texture.pitch, padded, texture.rows,
            texture.slice_size);
      }
    }
  }
  std::printf("linear textures: %u, padded layout fits %u\n", linear_textures,
              linear_fit);
  auto be32 = [](const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
           p[3];
  };
  uint32_t streams = 0;
  uint32_t objects = 0;
  std::vector<uint8_t> object(kAnimationObjectBytes);
  std::vector<uint8_t> mirrored(kAnimationObjectBytes);
  for (const Entry& entry : catalog.entries()) {
    std::vector<uint8_t> stream;
    if (!catalog.AnimationStream(entry.index, &stream)) {
      continue;
    }
    ++streams;
    object.assign(kAnimationObjectBytes, 0);
    object[kAnimationSizeOffset] = 0x01;
    mirrored = object;
    const bool written = WriteAnimationObject(stream, false, object.data());
    const bool flipped = WriteAnimationObject(stream, true, mirrored.data());
    uint32_t differing = 0;
    for (size_t i = 0; i < object.size(); ++i) {
      differing += object[i] != mirrored[i];
    }
    auto clip = catalog.LoadClip(entry.index);
    const uint8_t* o = object.data();
    const uint32_t size = be32(o + kAnimationSizeOffset);
    const bool ok = written && flipped && clip && be32(o) == clip->frames &&
                    be32(o + 4) == clip->joints &&
                    kAnimationHeaderBytes + size == stream.size();
    objects += ok;
    if (!ok || streams <= 3) {
      std::printf(
          "animation %u %s: %s, stream %zu, size %X, frames %u/%u joints "
          "%u/%u B %u/%u motion %u ints %u, ends %X %X %X %X, rate %08X, "
          "mirror flips %u bytes\n",
          entry.index, entry.name.c_str(), ok ? "ok" : "FAILED", stream.size(),
          size, be32(o), clip ? clip->frames : 0, be32(o + 4),
          clip ? clip->joints : 0, be32(o + 0x2BEC), be32(o + 0x2BF0),
          be32(o + 0x57DC), be32(o + 0x5920), be32(o + 0x597C),
          be32(o + 0x5980), be32(o + 0x5984), size, be32(o + 0x5968),
          differing);
    }
  }
  std::printf("animations: %u streams, %u objects ok\n", streams, objects);
  const uint64_t stock[] = {0x0040000000030003, 0x00400000002B0003,
                            0x0040000000130001, 0x0040000000260001,
                            0x0040000000090002, 0x00400000000C0002};
  for (uint64_t id : stock) {
    uint8_t guid[16] = {};
    for (int k = 0; k < 8; ++k) {
      guid[k] = uint8_t(id >> (56 - 8 * k));
    }
    const int32_t index = FindAnimation(catalog, guid);
    const Entry* entry = index >= 0 ? catalog.Find(uint32_t(index)) : nullptr;
    std::printf("stock %016llX -> %d %s\n", (unsigned long long)id, index,
                entry ? entry->name.c_str() : "-");
  }

  const Scene scene = BuildScene(catalog, description);
  const Expression expression;
  uint32_t max_bone = 0;
  for (const Part& part : scene.parts) {
    const Entry* entry = catalog.Find(part.entry);
    std::printf("part %u %s kind %08X: %zu batches, %zu textures\n", part.entry,
                entry ? entry->name.c_str() : "?", part.kind,
                part.model->batches.size(), part.model->textures.size());
    for (const Batch& batch : part.model->batches) {
      const Material material = BuildMaterial(scene, part, batch, expression);
      std::printf(
          "  batch shader %u uvs %u verts %zu tris %zu layers:", batch.shader,
          batch.uv_sets, batch.vertices.size(), batch.indices.size() / 3);
      for (uint32_t i = 0; i < kLayerCount; ++i) {
        if (material.layer[i][0]) {
          std::printf(" [k%u uv%u s%u c%u %ux%u]", material.layer[i][0],
                      material.layer[i][1], material.layer[i][2],
                      material.layer[i][3], material.textures[i]->width,
                      material.textures[i]->height);
        }
      }
      std::printf("\n");
      for (const Vertex& vertex : batch.vertices) {
        for (int k = 0; k < 4; ++k) {
          if (vertex.weights[k] > 0.0f && vertex.bones[k] > max_bone) {
            max_bone = vertex.bones[k];
          }
        }
      }
    }
  }
  for (uint32_t f = 0; f < kFeatureCount; ++f) {
    if (scene.features[f]) {
      std::printf("feature %u: entry %u %ux%u x%u\n", f,
                  scene.feature_entries[f], scene.features[f]->width,
                  scene.features[f]->height, scene.features[f]->slices);
    }
  }
  std::printf("highest weighted bone %u\n", max_bone);

  Matrix local[kMaxJoints];
  BindPose(MainSkeleton(), local);
  if (clip_index >= 0) {
    auto clip = catalog.LoadClip(uint32_t(clip_index));
    if (clip) {
      std::printf("clip %d: %u frames at %g fps, %u joints, %g s\n", clip_index,
                  clip->frames, clip->rate, clip->joints, clip->Length());
      SamplePose(*clip, MainSkeleton(), seconds, local);
    } else {
      std::printf("clip %d did not decode\n", clip_index);
    }
  }
  if (argc > 7) {
    FILE* bones = std::fopen(argv[7], "rb");
    if (bones) {
      for (uint32_t j = 0; j < kBoneCount; ++j) {
        std::fread(local[j].data(), sizeof(float), 16, bones);
      }
      std::fclose(bones);
      std::printf("bones from %s\n", argv[7]);
    }
  }
  std::vector<uint8_t> rgba;
  RenderPreview(scene, local, nullptr, expression, 360, 540, yaw, &rgba);
  const std::vector<uint8_t> png = EncodePng(360, 540, rgba);
  FILE* file = std::fopen(argv[2], "wb");
  if (!file) {
    return 1;
  }
  std::fwrite(png.data(), 1, png.size(), file);
  std::fclose(file);
  std::printf("wrote %s\n", argv[2]);
  return 0;
}
