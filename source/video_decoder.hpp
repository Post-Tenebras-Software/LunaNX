#pragma once
namespace luna::video
{
enum class Decoder { Software, Automatic };
enum class Backend { Software, Nvdec };
inline const char* decoderLabel(Decoder decoder)
{ return decoder == Decoder::Automatic ? "Auto (NVDEC)" : "Software"; }
inline const char* backendLabel(Backend backend)
{ return backend == Backend::Nvdec ? "NVDEC" : "software"; }
inline Decoder nextDecoder(Decoder decoder)
{ return decoder == Decoder::Automatic ? Decoder::Software : Decoder::Automatic; }
}
