// Copyright 2012 Google Inc. All Rights Reserved.
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

#include "syzygy/pe/decomposer.h"

#include "pcrecpp.h"  // NOLINT
#include "base/bind.h"
#include "base/stringprintf.h"
#include "base/utf_string_conversions.h"
#include "base/strings/string_split.h"
#include "base/win/scoped_bstr.h"
#include "base/win/scoped_comptr.h"
#include "syzygy/core/zstream.h"
#include "syzygy/pdb/omap.h"
#include "syzygy/pdb/pdb_byte_stream.h"
#include "syzygy/pdb/pdb_constants.h"
#include "syzygy/pdb/pdb_dbi_stream.h"
#include "syzygy/pdb/pdb_file.h"
#include "syzygy/pdb/pdb_reader.h"
#include "syzygy/pdb/pdb_symbol_record.h"
#include "syzygy/pdb/pdb_util.h"
#include "syzygy/pe/dia_util.h"
#include "syzygy/pe/find.h"
#include "syzygy/pe/pe_file_parser.h"
#include "syzygy/pe/pe_utils.h"
#include "syzygy/pe/serialization.h"
#include "third_party/cci/Files/CvInfo.h"

namespace cci = Microsoft_Cci_Pdb;

namespace {

using block_graph::BlockGraph;
using core::AbsoluteAddress;
using core::FileOffsetAddress;
using core::RelativeAddress;

typedef BlockGraph::Block Block;

// A small helper struct for dumping block information to log messages.
// TODO(chrisha): Move this to block_graph and reuse it everywhere!
struct BlockInfo {
  enum AddressType {
    kNoAddress,
    kAbsoluteAddress,
    kFileOffsetAddress,
    kRelativeAddress,
  };

  explicit BlockInfo(const Block* block)
      : block(block), type(kNoAddress) {
    DCHECK_NE(reinterpret_cast<Block*>(NULL), block);
  }

  BlockInfo(const Block* block,
            AbsoluteAddress address)
      : block(block), type(kAbsoluteAddress), abs_addr(address) {
    DCHECK_NE(reinterpret_cast<Block*>(NULL), block);
  }
  BlockInfo(const Block* block,
            FileOffsetAddress address)
      : block(block), type(kFileOffsetAddress), file_addr(address) {
    DCHECK_NE(reinterpret_cast<Block*>(NULL), block);
  }
  BlockInfo(const Block* block,
            RelativeAddress address)
      : block(block), type(kRelativeAddress), rel_addr(address) {
    DCHECK_NE(reinterpret_cast<Block*>(NULL), block);
  }

  const Block* block;
  AddressType type;

  // Ideally these would be in a union but because they have non-trivial
  // constructors they are not allowed.
  AbsoluteAddress abs_addr;
  FileOffsetAddress file_addr;
  RelativeAddress rel_addr;

 private:
  DISALLOW_COPY_AND_ASSIGN(BlockInfo);
};

}  // namespace

// Pretty prints a BlockInfo to an ostream. This has to be outside of any
// namespaces so that operator<< is found properly.
std::ostream& operator<<(std::ostream& os, const BlockInfo& bi) {
  os << "Block(id=" << bi.block->id() << ", name=\"" << bi.block->name()
     << "\", size=" << bi.block->size();
  if (bi.type != BlockInfo::kNoAddress) {
    os << ", address=";
    switch (bi.type) {
      case BlockInfo::kAbsoluteAddress: {
        os << bi.abs_addr;
        break;
      }
      case BlockInfo::kFileOffsetAddress: {
        os << bi.file_addr;
        break;
      }
      case BlockInfo::kRelativeAddress: {
        os << bi.rel_addr;
        break;
      }
      default: break;
    }
  }
  os << ")";
  return os;
}

namespace pe {

// An intermediate reference representation used while parsing PE blocks.
// This is necessary because at that point we haven't yet chunked the whole
// image into blocks thus some references cannot be resolved.
struct Decomposer::IntermediateReference {
  RelativeAddress src_addr;
  BlockGraph::ReferenceType type;
  BlockGraph::Size size;
  RelativeAddress dst_addr;
};

namespace {

using base::win::ScopedBstr;
using base::win::ScopedComPtr;
using builder::Callback;
using builder::Opt;
using builder::Or;
using builder::Seq;
using builder::Star;

typedef BlockGraph::BlockType BlockType;
typedef BlockGraph::Offset Offset;
typedef BlockGraph::Reference Reference;
typedef BlockGraph::ReferenceType ReferenceType;
typedef core::AddressRange<RelativeAddress, size_t> RelativeRange;
typedef Decomposer::IntermediateReference IntermediateReference;
typedef Decomposer::IntermediateReferences IntermediateReferences;
typedef pcrecpp::RE RE;
typedef std::vector<OMAP> OMAPs;
typedef std::vector<pdb::PdbFixup> PdbFixups;

const char kJumpTable[] = "<jump-table>";
const char kCaseTable[] = "<case-table>";

// The MS linker pads between code blocks with int3s.
static const uint8 kInt3 = 0xCC;
static const size_t kPointerSize = BlockGraph::Reference::kMaximumSize;

// Some helper functions for testing ranges.
template<typename T1, typename T2, typename T3>
bool InRange(T1 value, T2 lower_bound_incl, T3 length_excl) {
  T1 upper_bound_excl = static_cast<T1>(lower_bound_incl) + length_excl;
  return static_cast<T1>(lower_bound_incl) <= value &&
      value < static_cast<T2>(upper_bound_excl);
}
template<typename T1, typename T2, typename T3>
bool InRangeIncl(T1 value, T2 lower_bound_incl, T3 length_incl) {
  T1 upper_bound_incl = static_cast<T1>(lower_bound_incl) + length_incl;
  return static_cast<T1>(lower_bound_incl) <= value &&
      value <= upper_bound_incl;
}

bool InitializeDia(const PEFile& image_file,
                   const base::FilePath& pdb_path,
                   IDiaDataSource** dia_source,
                   IDiaSession** dia_session,
                   IDiaSymbol** global) {
  DCHECK_EQ(reinterpret_cast<IDiaDataSource*>(NULL), *dia_source);
  DCHECK_EQ(reinterpret_cast<IDiaSession*>(NULL), *dia_session);
  DCHECK_EQ(reinterpret_cast<IDiaSymbol*>(NULL), *global);

  if (!CreateDiaSource(dia_source))
    return false;
  DCHECK_NE(reinterpret_cast<IDiaDataSource*>(NULL), *dia_source);

  // We create the session using the PDB file directly, as we've already
  // validated that it matches the module.
  if (!CreateDiaSession(pdb_path, *dia_source, dia_session))
    return false;
  DCHECK_NE(reinterpret_cast<IDiaSession*>(NULL), *dia_session);

  HRESULT hr = (*dia_session)->get_globalScope(global);
  if (hr != S_OK) {
    LOG(ERROR) << "Failed to get the DIA global scope: "
               << common::LogHr(hr) << ".";
    return false;
  }

  return true;
}

// Given a compiland, returns its compiland details.
bool GetCompilandDetailsForCompiland(IDiaSymbol* compiland,
                                     IDiaSymbol** compiland_details) {
  DCHECK_NE(reinterpret_cast<IDiaSymbol*>(NULL), compiland);
  DCHECK_NE(reinterpret_cast<IDiaSymbol**>(NULL), compiland_details);
  DCHECK(IsSymTag(compiland, SymTagCompiland));
  DCHECK_EQ(reinterpret_cast<IDiaSymbol*>(NULL), *compiland_details);

  // Get the enumeration of compiland details.
  ScopedComPtr<IDiaEnumSymbols> enum_symbols;
  HRESULT hr = compiland->findChildren(SymTagCompilandDetails, NULL, 0,
                                       enum_symbols.Receive());
  DCHECK_EQ(S_OK, hr);

  // We expect there to be compiland details. For compilands built by
  // non-standard toolchains, there usually aren't any.
  LONG count = 0;
  hr = enum_symbols->get_Count(&count);
  DCHECK_EQ(S_OK, hr);
  if (count == 0) {
    // We don't log here because we see this quite often.
    return false;
  }

  // We do sometimes encounter more than one compiland detail. In fact, for
  // import and export tables we get one compiland detail per table entry.
  // They are all marked as having been generated by the linker, so using the
  // first one is sufficient.

  // Get the compiland details.
  ULONG fetched = 0;
  hr = enum_symbols->Next(1, compiland_details, &fetched);
  DCHECK_EQ(S_OK, hr);
  DCHECK_EQ(1u, fetched);

  return true;
}

// Stores information regarding known compilers.
struct KnownCompilerInfo {
  wchar_t* compiler_name;
  bool supported;
};

// A list of known compilers, and their status as being supported or not.
KnownCompilerInfo kKnownCompilerInfos[] = {
  { L"Microsoft (R) Macro Assembler", false },
  { L"Microsoft (R) Optimizing Compiler", true },
  { L"Microsoft (R) LINK", false }
};

// Given a compiland, determines whether the compiler used is one of those that
// we whitelist.
bool IsBuiltBySupportedCompiler(IDiaSymbol* compiland) {
  DCHECK_NE(reinterpret_cast<IDiaSymbol*>(NULL), compiland);
  DCHECK(IsSymTag(compiland, SymTagCompiland));

  ScopedComPtr<IDiaSymbol> compiland_details;
  if (!GetCompilandDetailsForCompiland(compiland,
                                       compiland_details.Receive())) {
    // If the compiland has no compiland details we assume the compiler is not
    // supported.
    ScopedBstr compiland_name;
    if (compiland->get_name(compiland_name.Receive()) == S_OK) {
      VLOG(1) << "Compiland has no compiland details: "
              << common::ToString(compiland_name);
    }
    return false;
  }
  DCHECK_NE(reinterpret_cast<IDiaSymbol*>(NULL), compiland_details.get());

  // Get the compiler name.
  ScopedBstr compiler_name;
  HRESULT hr = compiland_details->get_compilerName(compiler_name.Receive());
  DCHECK_EQ(S_OK, hr);

  // Check the compiler name against the list of known compilers.
  for (size_t i = 0; i < arraysize(kKnownCompilerInfos); ++i) {
    if (::wcscmp(kKnownCompilerInfos[i].compiler_name, compiler_name) == 0) {
      return kKnownCompilerInfos[i].supported;
    }
  }

  // Anything we don't explicitly know about is not supported.
  VLOG(1) << "Encountered unknown compiler: " << compiler_name;
  return false;
}

// Adds an intermediate reference to the provided vector. The vector is
// specified as the first parameter (in slight violation of our coding
// standards) because this function is intended to be used by Bind.
bool AddIntermediateReference(IntermediateReferences* references,
                              RelativeAddress src_addr,
                              ReferenceType type,
                              BlockGraph::Size size,
                              RelativeAddress dst_addr) {
  DCHECK_NE(reinterpret_cast<IntermediateReferences*>(NULL), references);
  IntermediateReference ref = { src_addr, type, size, dst_addr };
  references->push_back(ref);
  return true;
}

// Create a reference as specified. Ignores existing references if they are of
// the exact same type.
bool CreateReference(RelativeAddress src_addr,
                     BlockGraph::Size ref_size,
                     ReferenceType ref_type,
                     RelativeAddress base_addr,
                     RelativeAddress dst_addr,
                     BlockGraph::AddressSpace* image) {
  DCHECK_NE(reinterpret_cast<BlockGraph::AddressSpace*>(NULL), image);

  // Get the source block and offset, and ensure that the reference fits
  // within it.
  Block* src_block = image->GetBlockByAddress(src_addr);
  if (src_block == NULL) {
    LOG(ERROR) << "Unable to find block for reference originating at "
               << src_addr << ".";
    return false;
  }
  RelativeAddress src_block_addr;
  CHECK(image->GetAddressOf(src_block, &src_block_addr));
  Offset src_block_offset = src_addr - src_block_addr;
  if (src_block_offset + ref_size > src_block->size()) {
    LOG(ERROR) << "Reference originating at " << src_addr
               << " extends beyond block \"" << src_block->name() << "\".";
    return false;
  }

  // Get the destination block and offset.
  Block* dst_block = image->GetBlockByAddress(base_addr);
  if (dst_block == NULL) {
    LOG(ERROR) << "Unable to find block for reference pointing at "
               << base_addr << ".";
    return false;
  }
  RelativeAddress dst_block_addr;
  CHECK(image->GetAddressOf(dst_block, &dst_block_addr));
  Offset base = base_addr - dst_block_addr;
  Offset offset = dst_addr - dst_block_addr;

  Reference ref(ref_type, ref_size, dst_block, offset, base);

  // Check if a reference already exists at this offset.
  Block::ReferenceMap::const_iterator ref_it =
      src_block->references().find(src_block_offset);
  if (ref_it != src_block->references().end()) {
    // If an identical reference already exists then we're done.
    if (ref == ref_it->second)
      return true;
    LOG(ERROR) << "Block \"" << src_block->name() << "\" has a conflicting "
               << "reference at offset " << src_block_offset << ".";
    return false;
  }

  CHECK(src_block->SetReference(src_block_offset, ref));

  return true;
}

// Loads FIXUP and OMAP_FROM debug streams.
bool LoadDebugStreams(IDiaSession* dia_session,
                      PdbFixups* pdb_fixups,
                      OMAPs* omap_from) {
  DCHECK_NE(reinterpret_cast<IDiaSession*>(NULL), dia_session);
  DCHECK_NE(reinterpret_cast<PdbFixups*>(NULL), pdb_fixups);
  DCHECK_NE(reinterpret_cast<OMAPs*>(NULL), omap_from);

  // Load the fixups. These must exist.
  SearchResult search_result = FindAndLoadDiaDebugStreamByName(
      kFixupDiaDebugStreamName, dia_session, pdb_fixups);
  if (search_result != kSearchSucceeded) {
    if (search_result == kSearchFailed) {
      LOG(ERROR) << "PDB file does not contain a FIXUP stream. Module must be "
                    "linked with '/PROFILE' or '/DEBUGINFO:FIXUP' flag.";
    }
    return false;
  }

  // Load the omap_from table. It is not necessary that one exist.
  search_result = FindAndLoadDiaDebugStreamByName(
      kOmapFromDiaDebugStreamName, dia_session, omap_from);
  if (search_result == kSearchErrored) {
    LOG(ERROR) << "Error trying to read " << kOmapFromDiaDebugStreamName
               << " stream.";
    return false;
  }

  return true;
}

bool GetFixupDestinationAndType(const PEFile& image_file,
                                const pdb::PdbFixup& fixup,
                                RelativeAddress* dst_addr,
                                ReferenceType* ref_type) {
  DCHECK_NE(reinterpret_cast<RelativeAddress*>(NULL), dst_addr);
  DCHECK_NE(reinterpret_cast<ReferenceType*>(NULL), ref_type);

  RelativeAddress src_addr(fixup.rva_location);

  // Get the destination displacement from the actual image itself. We only see
  // fixups for 32-bit references.
  uint32 data = 0;
  if (!image_file.ReadImage(src_addr, &data, sizeof(data))) {
    LOG(ERROR) << "Unable to read image data for fixup with source address "
               << "at" << src_addr << ".";
    return false;
  }

  // Translate this to a relative displacement value.
  switch (fixup.type) {
    case pdb::PdbFixup::TYPE_ABSOLUTE: {
      *ref_type = BlockGraph::ABSOLUTE_REF;
      *dst_addr = RelativeAddress(image_file.AbsToRelDisplacement(data));
      break;
    }

    case pdb::PdbFixup::TYPE_PC_RELATIVE: {
      *ref_type = BlockGraph::PC_RELATIVE_REF;
      *dst_addr = RelativeAddress(fixup.rva_location) + sizeof(data) + data;
      break;
    }

    case pdb::PdbFixup::TYPE_RELATIVE: {
      *ref_type = BlockGraph::RELATIVE_REF;
      *dst_addr = RelativeAddress(data);
      break;
    }

    default: {
      LOG(ERROR) << "Unexpected fixup type (" << fixup.type << ").";
      return false;
    }
  }

  return true;
}

// Creates references from the @p pdb_fixups (translating them via the
// provided @p omap_from information if it is not empty), all while removing the
// corresponding entries from @p reloc_set. If @p reloc_set is not empty after
// this then the PDB fixups are out of sync with the image and we are unable to
// safely decompose.
//
// @note This function deliberately ignores fixup information for the resource
//     section. This is because chrome.dll gets modified by a manifest tool
//     which doesn't update the FIXUPs in the corresponding PDB. They are thus
//     out of sync. Even if they were in sync this doesn't harm us as we have no
//     need to reach in and modify resource data.
bool CreateReferencesFromFixupsImpl(
    const PEFile& image_file,
    const PdbFixups& pdb_fixups,
    const OMAPs& omap_from,
    PEFile::RelocSet* reloc_set,
    BlockGraph::AddressSpace* image) {
  DCHECK_NE(reinterpret_cast<PEFile::RelocSet*>(NULL), reloc_set);
  DCHECK_NE(reinterpret_cast<BlockGraph::AddressSpace*>(NULL), image);

  bool have_omap = !omap_from.empty();
  size_t fixups_used = 0;

  // The resource section in Chrome is modified post-link by a tool that adds a
  // manifest to it. This causes all of the fixups in the resource section (and
  // anything beyond it) to be invalid. As long as the resource section is the
  // last section in the image, this is not a problem (we can safely ignore the
  // .rsrc fixups, which we know how to parse without them). However, if there
  // is a section after the resource section, things will have been shifted
  // and potentially crucial fixups will be invalid.
  const IMAGE_SECTION_HEADER* rsrc_header = image_file.GetSectionHeader(
      kResourceSectionName);
  RelativeAddress rsrc_start(0xffffffff);
  RelativeAddress rsrc_end(0xffffffff);
  if (rsrc_header != NULL) {
    rsrc_start = RelativeAddress(rsrc_header->VirtualAddress);
    rsrc_end = rsrc_start + rsrc_header->Misc.VirtualSize;
  }

  // Ensure the fixups are all valid.
  for (size_t i = 0; i < pdb_fixups.size(); ++i) {
    if (!pdb_fixups[i].ValidHeader()) {
      LOG(ERROR) << "Unknown fixup header: "
                 << base::StringPrintf("0x%08X.", pdb_fixups[i].header);
      return false;
    }

    // For now, we skip any offset fixups. We've only seen this in the context
    // of TLS data access, and we don't mess with TLS structures.
    if (pdb_fixups[i].is_offset())
      continue;

    // All fixups we handle should be full size pointers.
    DCHECK_EQ(Reference::kMaximumSize, pdb_fixups[i].size());

    // Get the original addresses, and map them through OMAP information.
    // Normally DIA takes care of this for us, but there is no API for
    // getting DIA to give us FIXUP information, so we have to do it manually.
    RelativeAddress src_addr(pdb_fixups[i].rva_location);
    RelativeAddress base_addr(pdb_fixups[i].rva_base);
    if (have_omap) {
      src_addr = pdb::TranslateAddressViaOmap(omap_from, src_addr);
      base_addr = pdb::TranslateAddressViaOmap(omap_from, base_addr);
    }

    // If the reference originates beyond the .rsrc section then we can't
    // trust it.
    if (src_addr >= rsrc_end) {
      LOG(ERROR) << "Found fixup originating beyond .rsrc section.";
      return false;
    }

    // If the reference originates from a part of the .rsrc section, ignore it.
    if (src_addr >= rsrc_start)
      continue;

    // Get the relative address/displacement of the fixup. This logs on failure.
    RelativeAddress dst_addr;
    ReferenceType type = BlockGraph::RELATIVE_REF;
    if (!GetFixupDestinationAndType(image_file, pdb_fixups[i], &dst_addr,
                                    &type)) {
      return false;
    }

    // Finally, create the reference. This logs verbosely for us on failure.
    if (!CreateReference(src_addr, Reference::kMaximumSize, type, base_addr,
                         dst_addr, image)) {
      return false;
    }

    // Remove this reference from the relocs.
    PEFile::RelocSet::iterator reloc_it = reloc_set->find(src_addr);
    if (reloc_it != reloc_set->end()) {
      // We should only find a reloc if the fixup was of absolute type.
      if (type != BlockGraph::ABSOLUTE_REF) {
        LOG(ERROR) << "Found a reloc corresponding to a non-absolute fixup.";
        return false;
      }

      reloc_set->erase(reloc_it);
    }

    ++fixups_used;
  }

  return true;
}

bool GetDataSymbolSize(IDiaSymbol* symbol, size_t* length) {
  DCHECK_NE(reinterpret_cast<IDiaSymbol*>(NULL), symbol);
  DCHECK_NE(reinterpret_cast<size_t*>(NULL), length);

  *length = 0;
  ScopedComPtr<IDiaSymbol> type;
  HRESULT hr = symbol->get_type(type.Receive());
  // This happens if the symbol has no type information.
  if (hr == S_FALSE)
    return true;
  if (hr != S_OK) {
    LOG(ERROR) << "Failed to get type symbol: " << common::LogHr(hr) << ".";
    return false;
  }

  ULONGLONG ull_length = 0;
  hr = type->get_length(&ull_length);
  if (hr != S_OK) {
    LOG(ERROR) << "Failed to retrieve type length properties: "
               << common::LogHr(hr) << ".";
    return false;
  }
  DCHECK_LE(ull_length, 0xFFFFFFFF);
  *length = static_cast<size_t>(ull_length);

  return true;
}

bool ScopeSymTagToLabelProperties(enum SymTagEnum sym_tag,
                                  size_t scope_count,
                                  BlockGraph::LabelAttributes* attr,
                                  std::string* name) {
  DCHECK_NE(reinterpret_cast<BlockGraph::LabelAttributes*>(NULL), attr);
  DCHECK_NE(reinterpret_cast<std::string*>(NULL), name);

  switch (sym_tag) {
    case SymTagFuncDebugStart: {
      *attr = BlockGraph::DEBUG_START_LABEL;
      *name = "<debug-start>";
      return true;
    }
    case SymTagFuncDebugEnd: {
      *attr = BlockGraph::DEBUG_END_LABEL;
      *name = "<debug-end>";
      return true;
    }
    case SymTagBlock: {
      *attr = BlockGraph::SCOPE_START_LABEL;
      *name = base::StringPrintf("<scope-start-%d>", scope_count);
      return true;
    }
    default:
      return false;
  }
  return false;
}

// Reads the linker module symbol stream from the given PDB file. This should
// always exist as the last module.
scoped_refptr<pdb::PdbStream> GetLinkerSymbolStream(
    const pdb::PdbFile& pdb_file) {
  static const char kLinkerModuleName[] = "* Linker *";

  // Get the DBI stream.
  scoped_refptr<pdb::PdbStream> stream =
      pdb_file.GetStream(pdb::kDbiStream);
  if (stream.get() == NULL) {
    LOG(ERROR) << "PDB does not contain a DBI stream.";
    return false;
  }

  // Read the entire thing into memory before parsing it. This makes parsing
  // much faster.
  scoped_refptr<pdb::PdbByteStream> dbi_stream(new pdb::PdbByteStream());
  if (!dbi_stream->Init(stream)) {
    LOG(ERROR) << "Failed to read DBI stream.";
  }

  // Parse the DBI stream.
  pdb::DbiStream dbi;
  if (!dbi.Read(dbi_stream.get())) {
    LOG(ERROR) << "Unable to parse DBI stream.";
    return false;
  }

  if (dbi.modules().empty()) {
    LOG(ERROR) << "DBI stream contains no modules.";
    return false;
  }

  // The last module has always been observed to be the linker module.
  const pdb::DbiModuleInfo& linker = dbi.modules().back();
  if (linker.module_name() != kLinkerModuleName) {
    LOG(ERROR) << "Last module is not the linker module.";
    return false;
  }

  // Get the symbol stream.
  stream = pdb_file.GetStream(linker.module_info_base().stream);
  if (stream.get() == NULL) {
    LOG(ERROR) << "Unable to open linker symbol stream.";
    return false;
  }

  // Also read it entirely into memory for faster parsing.
  scoped_refptr<pdb::PdbByteStream> symbols(new pdb::PdbByteStream());
  if (!symbols->Init(stream)) {
    LOG(ERROR) << "Failed to read linker symbol stream.";
  }

  return symbols;
}

// Parses a symbol from a PDB symbol stream. The @p buffer is populated with the
// data and upon success this returns the symbol directly cast onto the
// @p buffer data. On failure this returns NULL.
template<typename SymbolType>
const SymbolType* ParseSymbol(uint16 symbol_length,
                              pdb::PdbStream* stream,
                              std::vector<uint8>* buffer) {
  DCHECK_NE(reinterpret_cast<pdb::PdbStream*>(NULL), stream);
  DCHECK_NE(reinterpret_cast<std::vector<uint8>*>(NULL), buffer);

  buffer->clear();

  if (symbol_length < sizeof(SymbolType)) {
    LOG(ERROR) << "Symbol too small for casting.";
    return NULL;
  }

  if (!stream->Read(buffer, symbol_length)) {
    LOG(ERROR) << "Failed to read symbol.";
    return NULL;
  }

  return reinterpret_cast<const SymbolType*>(buffer->data());
}

// If the given run of bytes consists of a single value repeated, returns that
// value. Otherwise, returns -1.
int RepeatedValue(const uint8* data, size_t size) {
  DCHECK_NE(reinterpret_cast<uint8*>(NULL), data);
  const uint8* data_end = data + size;
  uint8 value = *(data++);
  for (; data < data_end; ++data) {
    if (*data != value)
      return -1;
  }
  return value;
}

// Searches through the given image layout graph, and labels blocks that are
// simply padding blocks.
bool FindPaddingBlocks(ImageLayout* image_layout) {
  DCHECK_NE(reinterpret_cast<ImageLayout*>(NULL), image_layout);

  BlockGraph* block_graph = image_layout->blocks.graph();
  DCHECK_NE(reinterpret_cast<BlockGraph*>(NULL), block_graph);

  BlockGraph::BlockMap::iterator block_it =
      block_graph->blocks_mutable().begin();
  for (; block_it != block_graph->blocks_mutable().end(); ++block_it) {
    Block& block = block_it->second;

    // Padding blocks must not have any symbol information: no labels,
    // no references, no referrers, and they must be a gap block.
    if (block.labels().size() != 0 ||
        block.references().size() != 0 ||
        block.referrers().size() != 0 ||
        (block.attributes() & BlockGraph::GAP_BLOCK) == 0) {
      continue;
    }

    switch (block.type()) {
      // Code blocks should be fully defined and consist of only int3s.
      case BlockGraph::CODE_BLOCK: {
        if (block.data_size() != block.size() ||
            RepeatedValue(block.data(), block.data_size()) != kInt3)
          continue;
        break;
      }

      // Data blocks should be uninitialized or have fully defined data
      // consisting only of zeros.
      default: {
        DCHECK_EQ(BlockGraph::DATA_BLOCK, block.type());
        if (block.data_size() == 0)  // Uninitialized data blocks are padding.
          break;
        if (block.data_size() != block.size() ||
            RepeatedValue(block.data(), block.data_size()) != 0)
          continue;
      }
    }

    // If we fall through to this point, then the block is a padding block.
    block.set_attribute(BlockGraph::PADDING_BLOCK);
  }

  return true;
}

bool CodeBlockHasAlignedJumpTables(const Block* block) {
  DCHECK_NE(reinterpret_cast<Block*>(NULL), block);
  DCHECK_EQ(BlockGraph::CODE_BLOCK, block->type());

  // Iterate over the labels of this block looking for jump tables.
  bool has_jump_tables = false;
  Block::LabelMap::const_iterator label_it =
      block->labels().begin();
  for (; label_it != block->labels().end(); ++label_it) {
    if (!label_it->second.has_attributes(BlockGraph::JUMP_TABLE_LABEL))
      continue;

    has_jump_tables = true;

    // If the jump table is misaligned we can return false immediately.
    if (label_it->first % kPointerSize != 0)
      return false;
  }

  return has_jump_tables;
}

bool AlignCodeBlocksWithJumpTables(ImageLayout* image_layout) {
  DCHECK_NE(reinterpret_cast<ImageLayout*>(NULL), image_layout);

  BlockGraph::AddressSpace::RangeMapConstIter block_it =
      image_layout->blocks.begin();
  for (; block_it != image_layout->blocks.end(); ++block_it) {
    Block* block = block_it->second;

    // We only care about code blocks that are already aligned 0 mod 4 but
    // whose explicit alignment is currently less than that.
    if (block->type() != BlockGraph::CODE_BLOCK)
      continue;
    if (block->alignment() >= kPointerSize)
      continue;
    if (block_it->first.start().value() % kPointerSize != 0)
      continue;

    // Inspect them to see if they have aligned jump tables. If they do,
    // set the alignment of the block itself.
    if (CodeBlockHasAlignedJumpTables(block_it->second))
      block->set_alignment(kPointerSize);
  }

  return true;
}

void GuessDataBlockAlignment(uint32 max_alignment,
                             RelativeAddress block_rva,
                             Block* block) {
  DCHECK_NE(static_cast<Block*>(NULL), block);
  DCHECK_EQ(BlockGraph::DATA_BLOCK, block->type());
  uint32 alignment = block_rva.GetAlignment();
  // Cap the alignment.
  if (alignment > max_alignment)
    alignment = max_alignment;
  block->set_alignment(alignment);
}

void GuessDataBlockAlignments(const PEFile& pe_file,
                              ImageLayout* image_layout) {
  DCHECK_NE(static_cast<ImageLayout*>(NULL), image_layout);

  uint32 max_alignment = pe_file.nt_headers()->OptionalHeader.SectionAlignment;

  BlockGraph::AddressSpace::RangeMapConstIter it = image_layout->blocks.begin();
  for (; it != image_layout->blocks.end(); ++it) {
    RelativeAddress block_rva = it->first.start();
    BlockGraph::Block* block = it->second;
    if (block->type() != BlockGraph::DATA_BLOCK)
      continue;
    GuessDataBlockAlignment(max_alignment, block_rva, block);
  }
}

}  // namespace

// We use ", " as a separator between symbol names. We sometimes see commas
// in symbol names but do not see whitespace. Thus, this provides a useful
// separator that is also human friendly to read.
const char Decomposer::kLabelNameSep[] = ", ";

// This is by CreateBlocksFromCoffGroups to communicate shared state to
// VisitLinkerSymbol via the VisitSymbols helper function.
struct Decomposer::VisitLinkerSymbolContext {
  int current_group_index;
  std::string current_group_prefix;
  RelativeAddress current_group_start;

  // These are the set of patterns that indicate bracketing groups. They
  // should match both the opening and the closing symbol, and have at least
  // one match group returning the common prefix.
  std::vector<RE> bracketing_groups;

  VisitLinkerSymbolContext() : current_group_index(-1) {
    // Matches groups like: .CRT$XCA -> .CRT$XCZ
    bracketing_groups.push_back(RE("(\\.CRT\\$X.)[AZ]"));
    // Matches groups like: .rtc$IAA -> .rtc$IZZ
    bracketing_groups.push_back(RE("(\\.rtc\\$.*)(AA|ZZ)"));
    // Matches exactly: ATL$__a -> ATL$__z
    bracketing_groups.push_back(RE("(ATL\\$__)[az]"));
    // Matches exactly: .tls -> .tls$ZZZ
    bracketing_groups.push_back(RE("(\\.tls)(\\$ZZZ)?"));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(VisitLinkerSymbolContext);
};

Decomposer::Decomposer(const PEFile& image_file)
    : image_file_(image_file), image_layout_(NULL), image_(NULL),
      current_block_(NULL), current_scope_count_(0) {
}

bool Decomposer::Decompose(ImageLayout* image_layout) {
  DCHECK_NE(reinterpret_cast<ImageLayout*>(NULL), image_layout);

  // The temporaries should be NULL.
  DCHECK_EQ(reinterpret_cast<ImageLayout*>(NULL), image_layout_);
  DCHECK_EQ(reinterpret_cast<BlockGraph::AddressSpace*>(NULL), image_);

  // Set the image format.
  image_layout->blocks.graph()->set_image_format(BlockGraph::PE_IMAGE);

  // We start by finding the PDB path.
  if (!FindAndValidatePdbPath())
    return false;
  DCHECK(!pdb_path_.empty());

  // Load the serialized block-graph from the PDB if it exists. This allows
  // round-trip decomposition.
  bool stream_exists = false;
  if (LoadBlockGraphFromPdb(
          pdb_path_, image_file_, image_layout, &stream_exists)) {
    return true;
  } else if (stream_exists) {
    // If the stream exists but hasn't been loaded we return an error. At this
    // point an error message has already been logged if there was one.
    return false;
  }

  // At this point a full decomposition needs to be performed.
  image_layout_ = image_layout;
  image_ = &(image_layout->blocks);
  bool success = DecomposeImpl();
  image_layout_ = NULL;
  image_ = NULL;

  return success;
}

bool Decomposer::FindAndValidatePdbPath() {
  // Manually find the PDB path if it is not specified.
  if (pdb_path_.empty()) {
    if (!FindPdbForModule(image_file_.path(), &pdb_path_) ||
        pdb_path_.empty()) {
      LOG(ERROR) << "Unable to find PDB file for module: "
                 << image_file_.path().value();
      return false;
    }
  }
  DCHECK(!pdb_path_.empty());

  if (!file_util::PathExists(pdb_path_)) {
    LOG(ERROR) << "Path not found: " << pdb_path_.value();
    return false;
  }

  if (!pe::PeAndPdbAreMatched(image_file_.path(), pdb_path_)) {
    LOG(ERROR) << "PDB file \"" << pdb_path_.value() << "\" does not match "
               << "module \"" << image_file_.path().value() << "\".";
    return false;
  }

  return true;
}

bool Decomposer::LoadBlockGraphFromPdbStream(
    const PEFile& image_file,
    pdb::PdbStream* block_graph_stream,
    ImageLayout* image_layout) {
  DCHECK_NE(reinterpret_cast<pdb::PdbStream*>(NULL), block_graph_stream);
  DCHECK_NE(reinterpret_cast<ImageLayout*>(NULL), image_layout);
  LOG(INFO) << "Reading block-graph and image layout from the PDB.";

  // Initialize an input archive pointing to the stream.
  scoped_refptr<pdb::PdbByteStream> byte_stream = new pdb::PdbByteStream();
  if (!byte_stream->Init(block_graph_stream))
    return false;
  DCHECK_NE(reinterpret_cast<pdb::PdbByteStream*>(NULL), byte_stream.get());

  core::ScopedInStreamPtr pdb_in_stream;
  pdb_in_stream.reset(core::CreateByteInStream(
      byte_stream->data(), byte_stream->data() + byte_stream->length()));

  // Read the header.
  uint32 stream_version = 0;
  unsigned char compressed = 0;
  if (!pdb_in_stream->Read(sizeof(stream_version),
                           reinterpret_cast<core::Byte*>(&stream_version)) ||
      !pdb_in_stream->Read(sizeof(compressed),
                           reinterpret_cast<core::Byte*>(&compressed))) {
    LOG(ERROR) << "Failed to read existing Syzygy block-graph stream header.";
    return false;
  }

  // Check the stream version.
  if (stream_version != pdb::kSyzygyBlockGraphStreamVersion) {
    LOG(ERROR) << "PDB contains an unsupported Syzygy block-graph stream"
               << " version (got " << stream_version << ", expected "
               << pdb::kSyzygyBlockGraphStreamVersion << ").";
    return false;
  }

  // If the stream is compressed insert the decompression filter.
  core::InStream* in_stream = pdb_in_stream.get();
  scoped_ptr<core::ZInStream> zip_in_stream;
  if (compressed != 0) {
    zip_in_stream.reset(new core::ZInStream(in_stream));
    if (!zip_in_stream->Init()) {
      LOG(ERROR) << "Unable to initialize ZInStream.";
      return false;
    }
    in_stream = zip_in_stream.get();
  }

  // Deserialize the image-layout.
  core::NativeBinaryInArchive in_archive(in_stream);
  block_graph::BlockGraphSerializer::Attributes attributes = 0;
  if (!LoadBlockGraphAndImageLayout(
      image_file, &attributes, image_layout, &in_archive)) {
    LOG(ERROR) << "Failed to deserialize block-graph and image layout.";
    return false;
  }

  return true;
}

bool Decomposer::LoadBlockGraphFromPdb(const base::FilePath& pdb_path,
                                          const PEFile& image_file,
                                          ImageLayout* image_layout,
                                          bool* stream_exists) {
  DCHECK_NE(reinterpret_cast<ImageLayout*>(NULL), image_layout);
  DCHECK_NE(reinterpret_cast<bool*>(NULL), stream_exists);

  pdb::PdbFile pdb_file;
  pdb::PdbReader pdb_reader;
  if (!pdb_reader.Read(pdb_path, &pdb_file)) {
    LOG(ERROR) << "Unable to read the PDB named \"" << pdb_path.value()
               << "\".";
    return NULL;
  }

  // Try to get the block-graph stream from the PDB.
  scoped_refptr<pdb::PdbStream> block_graph_stream;
  if (!pdb::LoadNamedStreamFromPdbFile(pdb::kSyzygyBlockGraphStreamName,
                                       &pdb_file,
                                       &block_graph_stream) ||
      block_graph_stream.get() == NULL) {
    *stream_exists = false;
    return false;
  }
  if (block_graph_stream->length() == 0) {
    *stream_exists = false;
    LOG(WARNING) << "The block-graph stream is empty, ignoring it.";
    return false;
  }

  // The PDB contains a block-graph stream, the block-graph and the image layout
  // will be read from this stream.
  *stream_exists = true;
  if (!LoadBlockGraphFromPdbStream(image_file, block_graph_stream.get(),
                                   image_layout)) {
    return false;
  }

  return true;
}

bool Decomposer::DecomposeImpl() {
  // Instantiate and initialize our Debug Interface Access session. This logs
  // verbosely for us.
  ScopedComPtr<IDiaDataSource> dia_source;
  ScopedComPtr<IDiaSession> dia_session;
  ScopedComPtr<IDiaSymbol> global;
  if (!InitializeDia(image_file_, pdb_path_, dia_source.Receive(),
                     dia_session.Receive(), global.Receive())) {
    return false;
  }

  // Copy the image headers to the layout.
  CopySectionHeadersToImageLayout(
      image_file_.nt_headers()->FileHeader.NumberOfSections,
      image_file_.section_headers(),
      &(image_layout_->sections));

  // Create the sections in the underlying block-graph.
  if (!CopySectionInfoToBlockGraph(image_file_, image_->graph()))
    return false;

  // We scope the first few operations so that we don't keep the intermediate
  // references around any longer than we have to.
  {
    IntermediateReferences references;

    // First we parse out the PE blocks.
    VLOG(1) << "Parsing PE blocks.";
    if (!CreatePEImageBlocksAndReferences(&references))
      return false;

    // Now we parse the COFF group symbols from the linker's symbol stream.
    // These indicate things like static initializers, which must stay together
    // in a single block.
    VLOG(1) << "Parsing COFF groups.";
    if (!CreateBlocksFromCoffGroups())
      return false;

    // Next we parse out section contributions. Some of these may coincide with
    // existing PE parsed blocks, but when they do we expect them to be exact
    // collisions.
    VLOG(1) << "Parsing section contributions.";
    if (!CreateBlocksFromSectionContribs(dia_session.get()))
      return false;

    VLOG(1) << "Finding cold blocks.";
    if (!FindColdBlocksFromCompilands(dia_session.get()))
      return false;

    // Flesh out the rest of the image with gap blocks.
    VLOG(1) << "Creating gap blocks.";
    if (!CreateGapBlocks())
      return false;

    // Finalize the PE-parsed intermediate references.
    VLOG(1) << "Finalizing intermediate references.";
    if (!FinalizeIntermediateReferences(references))
      return false;
  }

  // Parse the fixups and use them to create references.
  VLOG(1) << "Parsing fixups.";
  if (!CreateReferencesFromFixups(dia_session.get()))
    return false;

  // Annotate the block-graph with symbol information.
  VLOG(1) << "Parsing symbols.";
  if (!ProcessSymbols(global.get()))
    return false;

  // Now, find and label any padding blocks.
  VLOG(1) << "Labeling padding blocks.";
  if (!FindPaddingBlocks(image_layout_))
    return false;

  // Set the alignment on code blocks with jump tables. This ensures that the
  // jump tables remain aligned post-transform.
  VLOG(1) << "Calculating code block alignments.";
  if (!AlignCodeBlocksWithJumpTables(image_layout_))
    return false;

  // Set the alignment of data blocks. This is not precise in that it simply
  // guesses the alignment based on the address of the block. Some instructions
  // have alignment requirements on their data but unfortunately the PDB does
  // not contain explicit alignment information.
  VLOG(1) << "Guessing data block alignments.";
  GuessDataBlockAlignments(image_file_, image_layout_);

  return true;
}

bool Decomposer::CreatePEImageBlocksAndReferences(
    IntermediateReferences* references) {
  DCHECK_NE(reinterpret_cast<IntermediateReferences*>(NULL), references);

  PEFileParser::AddReferenceCallback add_reference(
      base::Bind(&AddIntermediateReference, base::Unretained(references)));
  PEFileParser parser(image_file_, image_, add_reference);
  PEFileParser::PEHeader header;
  if (!parser.ParseImage(&header)) {
    LOG(ERROR) << "Unable to parse PE image.";
    return false;
  }

  return true;
}

bool Decomposer::CreateBlocksFromCoffGroups() {
  pdb::PdbFile pdb_file;
  pdb::PdbReader pdb_reader;
  if (!pdb_reader.Read(pdb_path_, &pdb_file)) {
    LOG(ERROR) << "Failed to load PDB: " << pdb_path_.value();
    return false;
  }

  scoped_refptr<pdb::PdbStream> symbols = GetLinkerSymbolStream(pdb_file);

  // Process the symbols in the linker module symbol stream.
  VisitLinkerSymbolContext context;
  pdb::VisitSymbolsCallback callback = base::Bind(
      &Decomposer::VisitLinkerSymbol,
      base::Unretained(this),
      base::Unretained(&context));
  if (!pdb::VisitSymbols(callback, symbols->length(), true, symbols.get()))
    return false;

  // Bail if we did not encounter a closing bracketing symbol where one was
  // expected.
  if (context.current_group_index != -1) {
    LOG(ERROR) << "Unable to close bracketed COFF group \""
               << context.current_group_prefix << "\".";
    return false;
  }

  return true;
}

bool Decomposer::CreateBlocksFromSectionContribs(IDiaSession* session) {
  ScopedComPtr<IDiaEnumSectionContribs> section_contribs;
  SearchResult search_result = FindDiaTable(session,
                                            section_contribs.Receive());
  if (search_result != kSearchSucceeded) {
    if (search_result == kSearchFailed)
      LOG(ERROR) << "No section contribution table found.";
    return false;
  }

  size_t rsrc_id = image_file_.GetSectionIndex(kResourceSectionName);

  LONG count = 0;
  if (section_contribs->get_Count(&count) != S_OK) {
    LOG(ERROR) << "Failed to get section contributions enumeration length.";
    return false;
  }

  for (LONG visited = 0; visited < count; ++visited) {
    ScopedComPtr<IDiaSectionContrib> section_contrib;
    ULONG fetched = 0;
    HRESULT hr = section_contribs->Next(1, section_contrib.Receive(), &fetched);
    // The standard way to end an enumeration (according to the docs) is by
    // returning S_FALSE and setting fetched to 0. We don't actually see this,
    // but it wouldn't be an error if we did.
    if (hr == S_FALSE && fetched == 0)
      break;
    if (hr != S_OK) {
      LOG(ERROR) << "Failed to get DIA section contribution: "
                 << common::LogHr(hr) << ".";
      return false;
    }
    // We actually end up seeing S_OK and fetched == 0 when the enumeration
    // terminates, which goes against the publishes documentations.
    if (fetched == 0)
      break;

    DWORD rva = 0;
    DWORD length = 0;
    DWORD section_id = 0;
    BOOL code = FALSE;
    ScopedComPtr<IDiaSymbol> compiland;
    ScopedBstr bstr_compiland_name;
    if ((hr = section_contrib->get_relativeVirtualAddress(&rva)) != S_OK ||
        (hr = section_contrib->get_length(&length)) != S_OK ||
        (hr = section_contrib->get_addressSection(&section_id)) != S_OK ||
        (hr = section_contrib->get_code(&code)) != S_OK ||
        (hr = section_contrib->get_compiland(compiland.Receive())) != S_OK ||
        (hr = compiland->get_name(bstr_compiland_name.Receive())) != S_OK) {
      LOG(ERROR) << "Failed to get section contribution properties: "
                 << common::LogHr(hr) << ".";
      return false;
    }

    // Determine if this function was built by a supported compiler.
    bool is_built_by_supported_compiler =
        IsBuiltBySupportedCompiler(compiland.get());

    // DIA numbers sections from 1 to n, while we do 0 to n - 1.
    DCHECK_LT(0u, section_id);
    --section_id;

    // We don't parse the resource section, as it is parsed by the PEFileParser.
    if (section_id == rsrc_id)
      continue;

    std::string compiland_name;
    if (!WideToUTF8(bstr_compiland_name, bstr_compiland_name.Length(),
                    &compiland_name)) {
      LOG(ERROR) << "Failed to convert compiland name to UTF8.";
      return false;
    }

    // Give a name to the block based on the basename of the object file. This
    // will eventually be replaced by the full symbol name, if one exists for
    // the block.
    size_t last_component = compiland_name.find_last_of('\\');
    size_t extension = compiland_name.find_last_of('.');
    if (last_component == std::string::npos) {
      last_component = 0;
    } else {
      // We don't want to include the last slash.
      ++last_component;
    }
    if (extension < last_component)
      extension = compiland_name.size();
    std::string name = compiland_name.substr(last_component,
                                             extension - last_component);

    // TODO(chrisha): We see special section contributions with the name
    //     "* CIL *". These are concatenations of data symbols and can very
    //     likely be chunked using symbols directly. A cursory visual inspection
    //     of symbol names hints that these might be related to WPO.

    // Create the block.
    BlockType block_type =
        code ? BlockGraph::CODE_BLOCK : BlockGraph::DATA_BLOCK;
    Block* block = CreateBlockOrFindCoveringPeBlock(
        block_type, RelativeAddress(rva), length, name);
    if (block == NULL) {
      LOG(ERROR) << "Unable to create block for compiland \""
                 << compiland_name << "\".";
      return false;
    }

    // Set the block compiland name.
    block->set_compiland_name(compiland_name);

    // Set the block attributes.
    block->set_attribute(BlockGraph::SECTION_CONTRIB);
    if (!is_built_by_supported_compiler)
      block->set_attribute(BlockGraph::BUILT_BY_UNSUPPORTED_COMPILER);
  }

  return true;
}

bool Decomposer::FindColdBlocksFromCompilands(IDiaSession* session) {
  // Detect hot/cold code separation. Some blocks are outside the function
  // address range and must be handled as separate blocks. When building
  // with PGO, the compiler can split functions into "hot" and "cold" blocks,
  // and move the "cold" blocks out to separate pages, so the function can be
  // noncontiguous.
  ScopedComPtr<IDiaSymbol> global;
  if (session->get_globalScope(global.Receive()) != S_OK) {
    LOG(ERROR) << "Cannot get global symbol.";
    return false;
  }

  // Find compilands within the global scope.
  ScopedComPtr<IDiaEnumSymbols> compilands;
  HRESULT status =
      global->findChildren(SymTagCompiland, NULL, 0, compilands.Receive());
  if (status != S_OK) {
    LOG(ERROR) << "Finding compilands failed on the global symbol: "
               << common::LogHr(status) << ".";
    return false;
  }

  // For each compiland, process its lexical blocks.
  while (true) {
    ULONG count = 0;
    ScopedComPtr<IDiaSymbol> compiland;
    if (compilands->Next(1, compiland.Receive(), &count) != S_OK ||
        count != 1) {
      break;
    }

    ScopedComPtr<IDiaEnumSymbols> compiland_blocks;
    status = compiland->findChildren(SymTagBlock,
                                     NULL,
                                     0,
                                     compiland_blocks.Receive());
    if (status != S_OK) {
      LOG(ERROR) << "Finding blocks failed on compiland: "
                 << common::LogHr(status) << ".";
      return false;
    }

    LONG blocks_count = 0;
    if (compiland_blocks->get_Count(&blocks_count) != S_OK) {
      LOG(ERROR) << "Failed to get compiland blocks enumeration length.";
      return false;
    }

    for (LONG block_index = 0; block_index < blocks_count; ++block_index) {
      ScopedComPtr<IDiaSymbol> compiland_block;
      ULONG fetched = 0;

      status = compiland_blocks->Next(1, compiland_block.Receive(), &fetched);
      if (status == S_FALSE && fetched == 0)
        break;
      if (status != S_OK) {
          LOG(ERROR) << "Failed to get function block: "
                     << common::LogHr(status) << ".";
        return false;
      }
      if (fetched == 0)
        break;

      ScopedComPtr<IDiaSymbol> parent;
      DWORD parent_tag = 0;
      if (compiland_block->get_lexicalParent(parent.Receive()) != S_OK ||
          parent->get_symTag(&parent_tag) != S_OK) {
        LOG(ERROR) << "Cannot retrieve block parent.";
        return false;
      }

      // Only consider function block.
      if (parent_tag != SymTagFunction)
        continue;

      // Get relative adresses.
      DWORD func_rva, block_rva;
      ULONGLONG func_length;
      if (compiland_block->get_relativeVirtualAddress(&block_rva) != S_OK ||
          parent->get_relativeVirtualAddress(&func_rva) != S_OK ||
          parent->get_length(&func_length) != S_OK) {
        LOG(ERROR) << "Cannot retrieve parent address range.";
        return false;
      }

      // Retrieve the function block.
      Block* func_block = image_->GetBlockByAddress(RelativeAddress(func_rva));
      if (func_block == NULL) {
        LOG(ERROR) << "Cannot retrieve parent block.";
        return false;
      }

      // Skip blocks within the range of its parent.
      if (block_rva >= func_rva && block_rva <= func_rva + func_length)
        continue;

      // A cold block is detected and needs special handling.
      Block* cold_block = image_->GetBlockByAddress(RelativeAddress(block_rva));
      if (cold_block == NULL) {
        LOG(ERROR) << "Cannot retrieve parent block.";
        return false;
      }

      RelativeAddress cold_block_addr;
      if (!image_->GetAddressOf(cold_block, &cold_block_addr)) {
        LOG(ERROR) << "Cannot retrieve cold block address.";
        return false;
      }

      // Add cold_block as a child of the function block.
      cold_blocks_[func_block][cold_block_addr] = cold_block;

      // Set the parent relation for blocks belonging to the function block.
      cold_blocks_parent_[func_block] = func_block;
      cold_blocks_parent_[cold_block] = func_block;
    }
  }

  return true;
}

bool Decomposer::CreateGapBlocks() {
  size_t num_sections = image_file_.nt_headers()->FileHeader.NumberOfSections;

  // Iterate through all the image sections.
  for (size_t i = 0; i < num_sections; ++i) {
    const IMAGE_SECTION_HEADER* header = image_file_.section_header(i);
    DCHECK_NE(reinterpret_cast<IMAGE_SECTION_HEADER*>(NULL), header);

    BlockType type = BlockGraph::CODE_BLOCK;
    const char* section_type = NULL;
    switch (GetSectionType(*header)) {
      case kSectionCode:
        type = BlockGraph::CODE_BLOCK;
        section_type = "code";
        break;

      case kSectionData:
        type = BlockGraph::DATA_BLOCK;
        section_type = "data";
        break;

      default:
        continue;
    }

    if (!CreateSectionGapBlocks(header, type)) {
      LOG(ERROR) << "Unable to create gap blocks for " << section_type
                 << " section \"" << header->Name << "\".";
      return false;
    }
  }

  return true;
}

bool Decomposer::FinalizeIntermediateReferences(
    const IntermediateReferences& references) {
  for (size_t i = 0; i < references.size(); ++i) {
    // This logs verbosely for us.
    if (!CreateReference(references[i].src_addr,
                         references[i].size,
                         references[i].type,
                         references[i].dst_addr,
                         references[i].dst_addr,
                         image_)) {
      return false;
    }
  }
  return true;
}

bool Decomposer::CreateReferencesFromFixups(IDiaSession* session) {
  DCHECK_NE(reinterpret_cast<IDiaSession*>(NULL), session);

  PEFile::RelocSet reloc_set;
  if (!image_file_.DecodeRelocs(&reloc_set))
    return false;

  OMAPs omap_from;
  PdbFixups fixups;
  if (!LoadDebugStreams(session, &fixups, &omap_from))
    return false;

  // While creating references from the fixups this removes the
  // corresponding reference data from the relocs. We use this as a kind of
  // double-entry bookkeeping to ensure all is well and right in the world.
  if (!CreateReferencesFromFixupsImpl(image_file_, fixups, omap_from,
                                      &reloc_set, image_)) {
    return false;
  }

  if (!reloc_set.empty()) {
    LOG(ERROR) << "Found reloc entries without matching FIXUP entries.";
    return false;
  }

  return true;
}

bool Decomposer::ProcessSymbols(IDiaSymbol* root) {
  DCHECK_NE(reinterpret_cast<IDiaSymbol*>(NULL), root);

  DiaBrowser::MatchCallback on_push_function_or_thunk_symbol(
      base::Bind(&Decomposer::OnPushFunctionOrThunkSymbol,
                 base::Unretained(this)));
  DiaBrowser::MatchCallback on_pop_function_or_thunk_symbol(
      base::Bind(&Decomposer::OnPopFunctionOrThunkSymbol,
                 base::Unretained(this)));
  DiaBrowser::MatchCallback on_function_child_symbol(
      base::Bind(&Decomposer::OnFunctionChildSymbol,
                 base::Unretained(this)));
  DiaBrowser::MatchCallback on_data_symbol(
      base::Bind(&Decomposer::OnDataSymbol, base::Unretained(this)));
  DiaBrowser::MatchCallback on_public_symbol(
      base::Bind(&Decomposer::OnPublicSymbol, base::Unretained(this)));
  DiaBrowser::MatchCallback on_label_symbol(
      base::Bind(&Decomposer::OnLabelSymbol, base::Unretained(this)));

  DiaBrowser dia_browser;

  // Find thunks.
  dia_browser.AddPattern(Seq(Opt(SymTagCompiland), SymTagThunk),
                         on_push_function_or_thunk_symbol,
                         on_pop_function_or_thunk_symbol);

  // Find functions and all data, labels, callsites, debug start/end and block
  // symbols below them. This is done in one single pattern so that the
  // function pushes/pops happen in the right order.
  dia_browser.AddPattern(
      Seq(Opt(SymTagCompiland),
          Callback(Or(SymTagFunction, SymTagThunk),
                   on_push_function_or_thunk_symbol,
                   on_pop_function_or_thunk_symbol),
          Star(SymTagBlock),
          Or(SymTagData,
             SymTagLabel,
             SymTagBlock,
             SymTagFuncDebugStart,
             SymTagFuncDebugEnd,
             SymTagCallSite)),
      on_function_child_symbol);

  // Global data and code label symbols.
  dia_browser.AddPattern(Seq(Opt(SymTagCompiland), SymTagLabel),
                         on_label_symbol);
  dia_browser.AddPattern(Seq(Opt(SymTagCompiland), SymTagData),
                         on_data_symbol);

  // Public symbols. These provide decorated names without any type info, but
  // are useful for debugging.
  dia_browser.AddPattern(SymTagPublicSymbol, on_public_symbol);

  return dia_browser.Browse(root);
}

bool Decomposer::VisitLinkerSymbol(VisitLinkerSymbolContext* context,
                                      uint16 symbol_length,
                                      uint16 symbol_type,
                                      pdb::PdbStream* stream) {
  DCHECK_NE(reinterpret_cast<VisitLinkerSymbolContext*>(NULL), context);
  DCHECK_NE(reinterpret_cast<pdb::PdbStream*>(NULL), stream);

  if (symbol_type != cci::S_COFFGROUP)
    return true;

  std::vector<uint8> buffer;
  const cci::CoffGroupSym* coffgroup =
      ParseSymbol<cci::CoffGroupSym>(symbol_length, stream, &buffer);
  if (coffgroup == NULL)
    return false;

  // The PDB numbers sections starting at index 1 but we use index 0.
  RelativeAddress rva(image_layout_->sections[coffgroup->seg - 1].addr +
      coffgroup->off);

  // We are looking for an opening symbol.
  if (context->current_group_index == -1) {
    for (size_t i = 0; i < context->bracketing_groups.size(); ++i) {
      std::string prefix;
      if (context->bracketing_groups[i].FullMatch(coffgroup->name, &prefix)) {
        context->current_group_index = i;
        context->current_group_prefix = prefix;
        context->current_group_start = rva;
        return true;
      }
    }

    // No opening symbol was encountered. We can safely ignore this
    // COFF group symbol.
    return true;
  }

  // If we get here we've found an opening symbol and we're looking for the
  // matching closing symbol.
  std::string prefix;
  if (!context->bracketing_groups[context->current_group_index].FullMatch(
          coffgroup->name, &prefix)) {
    return true;
  }

  if (prefix != context->current_group_prefix) {
    // We see another symbol open/close while already in an opened symbol.
    // This indicates nested bracketing information, which we've never seen
    // before.
    LOG(ERROR) << "Encountered nested bracket symbol \"" << prefix
               << "\" while in \"" << context->current_group_prefix << "\".";
    return false;
  }

  RelativeAddress end = rva + coffgroup->cb;
  DCHECK_LE(context->current_group_start, end);

  // If the COFF group is not empty, then create a block corresponding to it.
  if (context->current_group_start != end) {
    // Create a block for this bracketed COFF group.
    Block* block = CreateBlock(
        BlockGraph::DATA_BLOCK,
        context->current_group_start,
        end - context->current_group_start,
        base::StringPrintf("Bracketed COFF group: %s", prefix.c_str()));
    if (block == NULL) {
      LOG(ERROR) << "Failed to create bracketed COFF group \""
                 << prefix << "\".";
      return false;
    }
    block->set_attribute(BlockGraph::COFF_GROUP);
  }

  // Indicate that this block is closed and we're looking for another opening
  // bracket symbol.
  context->current_group_index = -1;
  context->current_group_prefix.clear();
  context->current_group_start = RelativeAddress(0);

  return true;
}

DiaBrowser::BrowserDirective Decomposer::OnPushFunctionOrThunkSymbol(
    const DiaBrowser& dia_browser,
    const DiaBrowser::SymTagVector& sym_tags,
    const DiaBrowser::SymbolPtrVector& symbols) {
  DCHECK(!symbols.empty());
  DCHECK_EQ(sym_tags.size(), symbols.size());
  DiaBrowser::SymbolPtr symbol = symbols.back();

  DCHECK_EQ(reinterpret_cast<Block*>(NULL), current_block_);
  DCHECK_EQ(current_address_, RelativeAddress(0));
  DCHECK_EQ(0u, current_scope_count_);

  HRESULT hr = E_FAIL;
  DWORD location_type = LocIsNull;
  DWORD rva = 0;
  ULONGLONG length = 0;
  ScopedBstr name_bstr;
  if (FAILED(hr = symbol->get_locationType(&location_type)) ||
      FAILED(hr = symbol->get_relativeVirtualAddress(&rva)) ||
      FAILED(hr = symbol->get_length(&length)) ||
      FAILED(hr = symbol->get_name(name_bstr.Receive()))) {
    LOG(ERROR) << "Failed to get function/thunk properties: "
               << common::LogHr(hr) << ".";
    return DiaBrowser::kBrowserAbort;
  }

  // We only care about functions with static storage. We can stop looking at
  // things below this node, as we won't be able to resolve them either.
  if (location_type != LocIsStatic)
    return DiaBrowser::kBrowserTerminatePath;

  RelativeAddress addr(rva);
  Block* block = image_->GetBlockByAddress(addr);
  CHECK(block != NULL);
  RelativeAddress block_addr;
  CHECK(image_->GetAddressOf(block, &block_addr));
  DCHECK(InRange(addr, block_addr, block->size()));

  std::string name;
  if (!WideToUTF8(name_bstr, name_bstr.Length(), &name)) {
    LOG(ERROR) << "Failed to convert function/thunk name to UTF8.";
    return DiaBrowser::kBrowserAbort;
  }

  // We know the function starts in this block but we need to make sure its
  // end does not extend past the end of the block.
  if (addr + length > block_addr + block->size()) {
    LOG(ERROR) << "Got function/thunk \"" << name << "\" that is not contained "
               << "by section contribution \"" << block->name() << "\".";
    return DiaBrowser::kBrowserAbort;
  }

  Offset offset = addr - block_addr;
  if (!AddLabelToBlock(offset, name, BlockGraph::CODE_LABEL, block))
    return DiaBrowser::kBrowserAbort;

  // Keep track of the generated block. We will use this when parsing symbols
  // that belong to this function. This prevents us from having to do repeated
  // lookups and also allows us to associate labels outside of the block to the
  // correct block.
  current_block_ = block;
  current_address_ = block_addr;

  // Certain properties are not defined on all blocks, so the following calls
  // may return S_FALSE.
  BOOL no_return = FALSE;
  if (symbol->get_noReturn(&no_return) != S_OK)
    no_return = FALSE;

  BOOL has_inl_asm = FALSE;
  if (symbol->get_hasInlAsm(&has_inl_asm) != S_OK)
    has_inl_asm = FALSE;

  BOOL has_eh = FALSE;
  if (symbol->get_hasEH(&has_eh) != S_OK)
    has_eh = FALSE;

  BOOL has_seh = FALSE;
  if (symbol->get_hasSEH(&has_seh) != S_OK)
    has_seh = FALSE;

  // Set the block attributes.
  if (no_return == TRUE)
    block->set_attribute(BlockGraph::NON_RETURN_FUNCTION);
  if (has_inl_asm == TRUE)
    block->set_attribute(BlockGraph::HAS_INLINE_ASSEMBLY);
  if (has_eh || has_seh)
    block->set_attribute(BlockGraph::HAS_EXCEPTION_HANDLING);
  if (IsSymTag(symbol, SymTagThunk))
    block->set_attribute(BlockGraph::THUNK);

  return DiaBrowser::kBrowserContinue;
}

DiaBrowser::BrowserDirective Decomposer::OnPopFunctionOrThunkSymbol(
    const DiaBrowser& dia_browser,
    const DiaBrowser::SymTagVector& sym_tags,
    const DiaBrowser::SymbolPtrVector& symbols) {
  // Simply clean up the current function block and address.
  current_block_ = NULL;
  current_address_ = RelativeAddress(0);
  current_scope_count_ = 0;
  return DiaBrowser::kBrowserContinue;
}

DiaBrowser::BrowserDirective Decomposer::OnFunctionChildSymbol(
      const DiaBrowser& dia_browser,
      const DiaBrowser::SymTagVector& sym_tags,
      const DiaBrowser::SymbolPtrVector& symbols) {
  DCHECK(!symbols.empty());
  DCHECK_EQ(sym_tags.size(), symbols.size());

  // This can only be called from the context of a function, so we expect the
  // parent function block to be set and remembered.
  DCHECK_NE(reinterpret_cast<Block*>(NULL), current_block_);

  // The set of sym tags here should match the pattern used in the DiaBrowser
  // instance set up in ProcessSymbols.
  switch (sym_tags.back()) {
    case SymTagData:
      return OnDataSymbol(dia_browser, sym_tags, symbols);

    case SymTagLabel:
      return OnLabelSymbol(dia_browser, sym_tags, symbols);

    case SymTagBlock:
    case SymTagFuncDebugStart:
    case SymTagFuncDebugEnd:
      return OnScopeSymbol(sym_tags.back(), symbols.back());

    case SymTagCallSite:
      return OnCallSiteSymbol(symbols.back());

    default:
      break;
  }

  LOG(ERROR) << "Unhandled function child symbol: " << sym_tags.back() << ".";
  return DiaBrowser::kBrowserAbort;
}

DiaBrowser::BrowserDirective Decomposer::OnDataSymbol(
    const DiaBrowser& dia_browser,
    const DiaBrowser::SymTagVector& sym_tags,
    const DiaBrowser::SymbolPtrVector& symbols) {
  DCHECK(!symbols.empty());
  DCHECK_EQ(sym_tags.size(), symbols.size());
  DiaBrowser::SymbolPtr symbol = symbols.back();

  HRESULT hr = E_FAIL;
  DWORD location_type = LocIsNull;
  DWORD rva = 0;
  ScopedBstr name_bstr;
  if (FAILED(hr = symbol->get_locationType(&location_type)) ||
      FAILED(hr = symbol->get_relativeVirtualAddress(&rva)) ||
      FAILED(hr = symbol->get_name(name_bstr.Receive()))) {
    LOG(ERROR) << "Failed to get data properties: " << common::LogHr(hr) << ".";
    return DiaBrowser::kBrowserAbort;
  }

  // Symbols with an address of zero are essentially invalid. They appear to
  // have been optimized away by the compiler, but they are still reported.
  if (rva == 0)
    return DiaBrowser::kBrowserTerminatePath;

  // We only care about functions with static storage. We can stop looking at
  // things below this node, as we won't be able to resolve them either.
  if (location_type != LocIsStatic)
    return DiaBrowser::kBrowserTerminatePath;

  // Get the size of this datum from its type info.
  size_t length = 0;
  if (!GetDataSymbolSize(symbol, &length))
    return DiaBrowser::kBrowserAbort;

  // Reuse the parent function block if we can. This acts as small lookup
  // cache.
  RelativeAddress addr(rva);
  Block* block = current_block_;
  RelativeAddress block_addr(current_address_);
  if (block == NULL || !InRange(addr, block_addr, block->size())) {
    block = image_->GetBlockByAddress(addr);
    CHECK(block != NULL);
    CHECK(image_->GetAddressOf(block, &block_addr));
    DCHECK(InRange(addr, block_addr, block->size()));
  }

  std::string name;
  if (!WideToUTF8(name_bstr, name_bstr.Length(), &name)) {
    LOG(ERROR) << "Failed to convert label name to UTF8.";
    return DiaBrowser::kBrowserAbort;
  }

  // Zero-length data symbols mark case/jump tables, or are forward declares.
  BlockGraph::LabelAttributes attr = BlockGraph::DATA_LABEL;
  Offset offset = addr - block_addr;
  if (length == 0) {
    // Jump and case tables come in as data symbols with no name. Jump tables
    // are always an array of pointers, thus they coincide exactly with a
    // reference. Case tables are simple arrays of integer values (themselves
    // indices into a jump table), thus do not coincide with a reference.
    if (name.empty() && block->type() == BlockGraph::CODE_BLOCK) {
      if (block->references().find(offset) != block->references().end()) {
        name = kJumpTable;
        attr |= BlockGraph::JUMP_TABLE_LABEL;
      } else {
        name = kCaseTable;
        attr |= BlockGraph::CASE_TABLE_LABEL;
      }
    } else {
      // Zero-length data symbols act as 'forward declares' in some sense. They
      // are always followed by a non-zero length data symbol with the same name
      // and location.
      return DiaBrowser::kBrowserTerminatePath;
    }
  }

  // Verify that the data symbol does not exceed the size of the block.
  if (addr + length > block_addr + block->size()) {
    // The data symbol can exceed the size of the block in the case of data
    // imports. For some reason the toolchain emits a global data symbol with
    // type information equal to the type of the data *pointed* to by the import
    // entry rather than the type of the entry itself. Thus, if the data type
    // is bigger than the entire IAT this symbol will exceed it. To complicate
    // matters even more, a poorly written module can import its own export in
    // which case a linker generated pseudo-import-entry block will be
    // generated. This won't be part of the IAT, so we can't even filter based
    // on that. Instead, we simply ignore global data symbols that exceed the
    // block size.
    base::StringPiece spname(name);
    if (sym_tags.size() == 1 && spname.starts_with("_imp_")) {
      VLOG(1) << "Encountered an imported data symbol \"" << name << "\" that "
              << "extends past its parent block \"" << block->name() << "\".";
    } else {
      LOG(ERROR) << "Received data symbol \"" << name << "\" that extends past "
                 << "its parent block \"" << block->name() << "\".";
      return DiaBrowser::kBrowserAbort;
    }
  }

  if (!AddLabelToBlock(offset, name, attr, block))
    return DiaBrowser::kBrowserAbort;

  return DiaBrowser::kBrowserContinue;
}

DiaBrowser::BrowserDirective Decomposer::OnPublicSymbol(
    const DiaBrowser& dia_browser,
    const DiaBrowser::SymTagVector& sym_tags,
    const DiaBrowser::SymbolPtrVector& symbols) {
  DCHECK(!symbols.empty());
  DCHECK_EQ(sym_tags.size(), symbols.size());
  DCHECK_EQ(reinterpret_cast<Block*>(NULL), current_block_);
  DiaBrowser::SymbolPtr symbol = symbols.back();

  HRESULT hr = E_FAIL;
  DWORD rva = 0;
  ScopedBstr name_bstr;
  if (FAILED(hr = symbol->get_relativeVirtualAddress(&rva)) ||
      FAILED(hr = symbol->get_name(name_bstr.Receive()))) {
    LOG(ERROR) << "Failed to get public symbol properties: "
               << common::LogHr(hr) << ".";
    return DiaBrowser::kBrowserAbort;
  }

  RelativeAddress addr(rva);
  Block* block = image_->GetBlockByAddress(addr);
  CHECK(block != NULL);
  RelativeAddress block_addr;
  CHECK(image_->GetAddressOf(block, &block_addr));
  DCHECK(InRange(addr, block_addr, block->size()));

  std::string name;
  WideToUTF8(name_bstr, name_bstr.Length(), &name);

  // Public symbol names are mangled. Remove leading '_' as per
  // http://msdn.microsoft.com/en-us/library/00kh39zz(v=vs.80).aspx
  if (name[0] == '_')
    name = name.substr(1);

  Offset offset = addr - block_addr;
  if (!AddLabelToBlock(offset, name, BlockGraph::PUBLIC_SYMBOL_LABEL, block))
    return DiaBrowser::kBrowserAbort;

  return DiaBrowser::kBrowserContinue;
}

DiaBrowser::BrowserDirective Decomposer::OnLabelSymbol(
    const DiaBrowser& dia_browser,
    const DiaBrowser::SymTagVector& sym_tags,
    const DiaBrowser::SymbolPtrVector& symbols) {
  DCHECK(!symbols.empty());
  DCHECK_EQ(sym_tags.size(), symbols.size());
  DiaBrowser::SymbolPtr symbol = symbols.back();

  HRESULT hr = E_FAIL;
  DWORD rva = 0;
  ScopedBstr name_bstr;
  if (FAILED(hr = symbol->get_relativeVirtualAddress(&rva)) ||
      FAILED(hr = symbol->get_name(name_bstr.Receive()))) {
    LOG(ERROR) << "Failed to get label symbol properties: " << common::LogHr(hr)
               << ".";
    return DiaBrowser::kBrowserAbort;
  }

  // If we have a current_block_ the label should lie within its scope.
  RelativeAddress addr(rva);
  Block* block = current_block_;
  RelativeAddress block_addr(current_address_);
  if (block != NULL) {
    // Try to find the block in the cold blocks. The cold blocks aren't in the
    // same address space as the original function.
    if (!InRangeIncl(addr, block_addr, block->size())) {

      // Determine the function block containing this block.
      ColdBlocksParent::iterator function_block =
          cold_blocks_parent_.find(block);
      if (function_block != cold_blocks_parent_.end())
        block = function_block->second;

      // Retrieve the first cold block related to that function before |addr|.
      ColdBlocksMap::iterator cold_blocks_it = cold_blocks_.find(block);
      if (cold_blocks_it != cold_blocks_.end()) {
        ColdBlocks& cold_blocks = cold_blocks_it->second;
        if (!cold_blocks.empty()) {
          // Find the block containing the address |addr|. When |addr| is not
          // the same as the block address, the iterator points to the next
          // block.
          ColdBlocks::iterator cold_block_it = cold_blocks.lower_bound(addr);
          if (cold_block_it == cold_blocks.end() ||
              cold_block_it->second->addr() != addr) {
            cold_block_it--;
          }

          // Check whether the address falls into this cold block.
          DCHECK(cold_block_it != cold_blocks.end());
          Block* cold_block = cold_block_it->second;
          if (InRangeIncl(addr, cold_block->addr(), cold_block->size()))
            block = cold_block;
        }
      }

      // Update the block address according to the cold block found.
      if (!image_->GetAddressOf(block, &block_addr)) {
        LOG(ERROR) << "Cannot retrieve cold block address.";
        return DiaBrowser::kBrowserAbort;
      }
    }

    if (!InRangeIncl(addr, block_addr, block->size())) {
      LOG(ERROR) << "Label falls outside of current block \""
                 << block->name() << "\".";
      return DiaBrowser::kBrowserAbort;
    }
  } else {
    // If there is no current block this is a compiland scope label.
    block = image_->GetBlockByAddress(addr);
    CHECK(block != NULL);
    CHECK(image_->GetAddressOf(block, &block_addr));
    DCHECK(InRange(addr, block_addr, block->size()));

    // TODO(chrisha): This label is in compiland scope, so we should be
    //     finding the block whose section contribution shares the same
    //     compiland.
  }

  std::string name;
  WideToUTF8(name_bstr, name_bstr.Length(), &name);

  Offset offset = addr - block_addr;
  if (!AddLabelToBlock(offset, name, BlockGraph::CODE_LABEL, block))
    return DiaBrowser::kBrowserAbort;

  return DiaBrowser::kBrowserContinue;
}

DiaBrowser::BrowserDirective Decomposer::OnScopeSymbol(
    enum SymTagEnum type, DiaBrowser::SymbolPtr symbol) {
  // We should only get here via the successful exploration of a SymTagFunction,
  // so current_block_ should be set.
  DCHECK_NE(reinterpret_cast<Block*>(NULL), current_block_);

  HRESULT hr = E_FAIL;
  DWORD rva = 0;
  if (FAILED(hr = symbol->get_relativeVirtualAddress(&rva))) {
    LOG(ERROR) << "Failed to get scope symbol properties: " << common::LogHr(hr)
               << ".";
    return DiaBrowser::kBrowserAbort;
  }

  // The label may potentially lay at the first byte past the function.
  RelativeAddress addr(rva);
  DCHECK_LE(current_address_, addr);
  DCHECK_LE(addr, current_address_ + current_block_->size());

  // Get the attributes for this label.
  BlockGraph::LabelAttributes attr = 0;
  std::string name;
  CHECK(ScopeSymTagToLabelProperties(type, current_scope_count_, &attr, &name));

  // Add the label.
  Offset offset = addr - current_address_;
  if (!AddLabelToBlock(offset, name, attr, current_block_))
    return DiaBrowser::kBrowserAbort;

  // If this is a scope we extract the length and explicitly add a corresponding
  // end label.
  if (type == SymTagBlock) {
    ULONGLONG length = 0;
    if (symbol->get_length(&length) != S_OK) {
      LOG(ERROR) << "Failed to extract code scope length for block \""
                  << current_block_->name() << "\".";
      return DiaBrowser::kBrowserAbort;
    }
    DCHECK_LE(static_cast<size_t>(offset + length), current_block_->size());
    name = base::StringPrintf("<scope-end-%d>", current_scope_count_);
    ++current_scope_count_;
    if (!AddLabelToBlock(offset + length, name,
                         BlockGraph::SCOPE_END_LABEL, current_block_)) {
      return DiaBrowser::kBrowserAbort;
    }
  }

  return DiaBrowser::kBrowserContinue;
}

DiaBrowser::BrowserDirective Decomposer::OnCallSiteSymbol(
    DiaBrowser::SymbolPtr symbol) {
  // We should only get here via the successful exploration of a SymTagFunction,
  // so current_block_ should be set.
  DCHECK_NE(reinterpret_cast<Block*>(NULL), current_block_);

  HRESULT hr = E_FAIL;
  DWORD rva = 0;
  if (FAILED(hr = symbol->get_relativeVirtualAddress(&rva))) {
    LOG(ERROR) << "Failed to get call site symbol properties: "
               << common::LogHr(hr) << ".";
    return DiaBrowser::kBrowserAbort;
  }

  RelativeAddress addr(rva);
  if (!InRange(addr, current_address_, current_block_->size())) {
    // We see this happen under some build configurations (notably debug
    // component builds of Chrome). As long as the label falls entirely
    // outside of the block it is harmless and can be safely ignored.
    VLOG(1) << "Call site falls outside of current block \""
            << current_block_->name() << "\".";
    return DiaBrowser::kBrowserContinue;
  }

  Offset offset = addr - current_address_;
  if (!AddLabelToBlock(offset, "<call-site>", BlockGraph::CALL_SITE_LABEL,
                       current_block_)) {
    return DiaBrowser::kBrowserAbort;
  }

  return DiaBrowser::kBrowserContinue;
}

Block* Decomposer::CreateBlock(BlockType type,
                                  RelativeAddress address,
                                  BlockGraph::Size size,
                                  const base::StringPiece& name) {
  Block* block = image_->AddBlock(type, address, size, name);
  if (block == NULL) {
    LOG(ERROR) << "Unable to add block \"" << name.as_string() << "\" at "
               << address << " with size " << size << ".";
    return NULL;
  }

  // Mark the source range from whence this block originates. This is assuming
  // an untransformed image. To handle transformed images we'd have to use the
  // OMAP information to do this properly.
  bool pushed = block->source_ranges().Push(
      Block::DataRange(0, size),
      Block::SourceRange(address, size));
  DCHECK(pushed);

  BlockGraph::SectionId section = image_file_.GetSectionIndex(address, size);
  if (section == BlockGraph::kInvalidSectionId) {
    LOG(ERROR) << "Block \"" << name.as_string() << "\" at " << address
               << " with size " << size << " lies outside of all sections.";
    return NULL;
  }
  block->set_section(section);

  const uint8* data = image_file_.GetImageData(address, size);
  if (data != NULL)
    block->SetData(data, size);

  return block;
}

Block* Decomposer::CreateBlockOrFindCoveringPeBlock(
    BlockType type,
    RelativeAddress addr,
    BlockGraph::Size size,
    const base::StringPiece& name) {
  Block* block = image_->GetBlockByAddress(addr);
  if (block != NULL) {
    RelativeAddress block_addr;
    CHECK(image_->GetAddressOf(block, &block_addr));

    // Allow PE-parsed blocks to be grown to reflect reality. For example,
    // in VS2013 the linker makes space for 2 debug directories rather than
    // just one, and the symbols reflect this. We parse the debug directory
    // with the size indicated in the PE header, which conflicts with that
    // indicated by the section contributions.
    if (name == "* Linker *" && block_addr == addr && size > block->size()) {
      if (!image_->ResizeBlock(block, size)) {
        LOG(ERROR) << "Failed to extend PE-parsed "
                   << BlockInfo(block, block_addr) << " with linker "
                   << "section contribution of size " << size << ".";

        // Get the conflicting block and output additional information about
        // it.
        Block* conflict = image_->GetFirstIntersectingBlock(
            block_addr + block->size(), size - block->size());
        if (conflict) {
          RelativeAddress conflict_addr;
          CHECK(image_->GetAddressOf(conflict, &conflict_addr));
          LOG(ERROR) << "Conflicts with existing "
                     << BlockInfo(conflict, conflict_addr) << ".";
        }

        return NULL;
      }

      // Update the data in the extended block.
      const uint8* data = image_file_.GetImageData(addr, size);
      block->SetData(data, size);
      return block;
    }

    // If this is not a PE parsed or COFF group block that covers us entirely,
    // then this is an error.
    static const BlockGraph::BlockAttributes kCoveringAttributes =
        BlockGraph::PE_PARSED | BlockGraph::COFF_GROUP;
    RelativeRange existing_block(block_addr, block->size());
    if ((block->attributes() & kCoveringAttributes) == 0 ||
        !existing_block.Contains(addr, size)) {
      LOG(ERROR) << "Trying to create block \"" << name.as_string() << "\" at "
                 << addr.value() << " with size " << size << " that conflicts "
                 << "with existing " << BlockInfo(block, block_addr) << ".";
      return NULL;
    }

    return block;
  }
  DCHECK_EQ(reinterpret_cast<Block*>(NULL), block);

  return CreateBlock(type, addr, size, name);
}

bool Decomposer::CreateGapBlock(BlockType block_type,
                                   RelativeAddress address,
                                   BlockGraph::Size size) {
  Block* block = CreateBlock(block_type, address, size,
      base::StringPrintf("Gap Block 0x%08X", address.value()).c_str());
  if (block == NULL) {
    LOG(ERROR) << "Unable to create gap block.";
    return false;
  }
  block->set_attribute(BlockGraph::GAP_BLOCK);

  return true;
}

bool Decomposer::CreateSectionGapBlocks(const IMAGE_SECTION_HEADER* header,
                                           BlockType block_type) {
  RelativeAddress section_begin(header->VirtualAddress);
  RelativeAddress section_end(section_begin + header->Misc.VirtualSize);
  RelativeAddress image_end(
      image_file_.nt_headers()->OptionalHeader.SizeOfImage);

  // Search for the first and last blocks interesting from the start and end
  // of the section to the end of the image.
  BlockGraph::AddressSpace::RangeMap::const_iterator it(
      image_->address_space_impl().FindFirstIntersection(
          BlockGraph::AddressSpace::Range(section_begin,
                                          image_end - section_begin)));

  BlockGraph::AddressSpace::RangeMap::const_iterator end =
      image_->address_space_impl().end();
  if (section_end < image_end) {
    end = image_->address_space_impl().FindFirstIntersection(
        BlockGraph::AddressSpace::Range(section_end,
                                        image_end - section_end));
  }

  // The whole section is missing. Cover it with one gap block.
  if (it == end)
    return CreateGapBlock(
        block_type, section_begin, section_end - section_begin);

  // Create the head gap block if need be.
  if (section_begin < it->first.start()) {
    if (!CreateGapBlock(
        block_type, section_begin, it->first.start() - section_begin)) {
      return false;
    }
  }

  // Now iterate the blocks and fill in gaps.
  for (; it != end; ++it) {
    const Block* block = it->second;
    DCHECK_NE(reinterpret_cast<Block*>(NULL), block);
    RelativeAddress block_end = it->first.start() + block->size();
    if (block_end >= section_end)
      break;

    // Walk to the next address in turn.
    BlockGraph::AddressSpace::RangeMap::const_iterator next = it;
    ++next;
    if (next == end) {
      // We're at the end of the list. Create the tail gap block.
      DCHECK_GT(section_end, block_end);
      if (!CreateGapBlock(block_type, block_end, section_end - block_end))
        return false;
      break;
    }

    // Create the interstitial gap block.
    if (block_end < next->first.start())
      if (!CreateGapBlock(
          block_type, block_end, next->first.start() - block_end)) {
        return false;
      }
  }

  return true;
}

}  // namespace pe
