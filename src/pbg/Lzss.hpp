#pragma once

#include "diffbuild.hpp"
#include "inttypes.hpp"
#include <windows.h>

#define LZSS_OFFSET_BITS 13
#define LZSS_LENGTH_BITS 4
#define LZSS_DICTSIZE (1 << LZSS_OFFSET_BITS)

namespace th08
{
#if defined(TH08_PSP_PORT)
class IPbgFile;
#endif

class Lzss
{
  public:
    // When out is supplied, encode into caller-owned storage.  The encoding
    // algorithm and byte stream are identical; PSP score serialization uses
    // this to reuse idle stage-pool storage instead of allocating a second
    // large temporary from a fragmented heap.
    static LPBYTE Encode(u8 *in, i32 inSize, i32 *outSize,
                         u8 *out = NULL, i32 outCapacity = 0);
    static LPBYTE Decode(u8 *in, i32 inSize, u8 *out, i32 outSize);
#if defined(TH08_PSP_PORT)
    // Decode directly from the archive file so the compressed entry does not
    // have to coexist with its decompressed destination in Main RAM.
    static LPBYTE DecodeFile(IPbgFile *in, u32 inSize, u8 *out, u32 outSize);

    // A streaming decode owns its dictionary and output coalescing buffer via
    // this caller-provided workspace.  The workspace must remain exclusively
    // owned by one DecodeFileToSink call until that call returns.  Unlike the
    // legacy APIs, streaming decodes do not touch Lzss::m_Dict and can run in
    // parallel when their IPbgFile instances, sinks and workspaces are also
    // independent.
    enum
    {
        STREAM_OUTPUT_BUFFER_SIZE = 4096
    };

    struct StreamWorkspace
    {
        u8 dictionary[LZSS_DICTSIZE];
        u8 output[STREAM_OUTPUT_BUFFER_SIZE];
    };

    // The sink returns the number of bytes it consumed synchronously.  data is
    // backed by StreamWorkspace::output and is valid only until the callback
    // returns.  A value other than dataSize, including a short write, aborts
    // the decode without retrying.  The sink can therefore keep parser state
    // transactional and report failure without a second decompressed buffer.
    typedef u32 (*StreamSinkWrite)(void *context, const u8 *data, u32 dataSize);

    enum StreamResult
    {
        STREAM_SUCCESS = 0,
        STREAM_INVALID_ARGUMENT,
        STREAM_INPUT_READ_FAILED,
        STREAM_OUTPUT_OVERFLOW,
        STREAM_OUTPUT_SIZE_MISMATCH,
        STREAM_SINK_WRITE_FAILED
    };

    // Decode a declared compressed entry into ordered sink chunks without
    // allocating its full output.  Success means exactly inSize bytes were
    // read from the file, exactly outSize bytes were produced and every byte
    // was accepted by the sink.  On failure the sink may already have received
    // a prefix; callers must keep that prefix private and roll it back.  The
    // input cursor is guaranteed to be at the end of the declared entry only
    // on success and is otherwise unspecified.
    static StreamResult DecodeFileToSink(IPbgFile *in, u32 inSize, u32 outSize,
                                         StreamSinkWrite sink, void *sinkContext,
                                         StreamWorkspace *workspace);
#endif

    static void InitTree(i32 root);
    static void InitEncoderState();
    static i32 AddString(i32 newNode, i32 *matchPosition);
    static void DeleteString(i32 p);
    static void ContractNode(i32 oldNode, i32 newNode);
    static void ReplaceNode(i32 oldNode, i32 newNode);
    static i32 FindNextNode(i32 node);

  private:
    struct TreeNode
    {
        i32 parent;
        i32 left;
        i32 right;
    };

    static TreeNode m_Tree[LZSS_DICTSIZE + 1];
    static u8 m_Dict[LZSS_DICTSIZE];
};
}; // namespace th08
