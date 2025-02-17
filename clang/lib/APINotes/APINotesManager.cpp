//===--- APINotesManager.cpp - Manage API Notes Files ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements the APINotesManager class.
//
//===----------------------------------------------------------------------===//

#include "clang/APINotes/APINotesManager.h"
#include "clang/APINotes/APINotesOptions.h"
#include "clang/APINotes/APINotesReader.h"
#include "clang/APINotes/APINotesYAMLCompiler.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/SourceMgrAdapter.h"
#include "clang/Basic/Version.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include <sys/stat.h>

using namespace clang;
using namespace api_notes;

#define DEBUG_TYPE "API Notes"
STATISTIC(NumHeaderAPINotes,
          "non-framework API notes files loaded");
STATISTIC(NumPublicFrameworkAPINotes,
          "framework public API notes loaded");
STATISTIC(NumPrivateFrameworkAPINotes,
          "framework private API notes loaded");
STATISTIC(NumFrameworksSearched,
          "frameworks searched");
STATISTIC(NumDirectoriesSearched,
          "header directories searched");
STATISTIC(NumDirectoryCacheHits,
          "directory cache hits");

namespace {
  /// Prints two successive strings, which much be kept alive as long as the
  /// PrettyStackTrace entry.
  class PrettyStackTraceDoubleString : public llvm::PrettyStackTraceEntry {
    StringRef First, Second;
  public:
    PrettyStackTraceDoubleString(StringRef first, StringRef second)
        : First(first), Second(second) {}
    void print(raw_ostream &OS) const override {
      OS << First << Second;
    }
  };
}

APINotesManager::APINotesManager(SourceManager &sourceMgr,
                                 const LangOptions &langOpts)
  : SourceMgr(sourceMgr), ImplicitAPINotes(langOpts.APINotes) { }

APINotesManager::~APINotesManager() {
  // Free the API notes readers.
  for (const auto &entry : Readers) {
    if (auto reader = entry.second.dyn_cast<APINotesReader *>()) {
      delete reader;
    }
  }

  delete CurrentModuleReaders[0];
  delete CurrentModuleReaders[1];
}

std::unique_ptr<APINotesReader>
APINotesManager::loadAPINotes(const FileEntry *apiNotesFile) {
  PrettyStackTraceDoubleString trace("Loading API notes from ",
                                     apiNotesFile->getName());

  // Open the source file.
  auto sourceFileID = SourceMgr.createFileID(apiNotesFile, SourceLocation(), SrcMgr::C_User);
  auto sourceBuffer = SourceMgr.getBufferOrNone(sourceFileID, SourceLocation());
  if (!sourceBuffer) return nullptr;

  // Compile the API notes source into a buffer.
  // FIXME: Either propagate OSType through or, better yet, improve the binary
  // APINotes format to maintain complete availability information.
  // FIXME: We don't even really need to go through the binary format at all;
  // we're just going to immediately deserialize it again.
  llvm::SmallVector<char, 1024> apiNotesBuffer;
  std::unique_ptr<llvm::MemoryBuffer> compiledBuffer;
  {
    SourceMgrAdapter srcMgrAdapter(SourceMgr, SourceMgr.getDiagnostics(),
                                   diag::err_apinotes_message,
                                   diag::warn_apinotes_message,
                                   diag::note_apinotes_message,
                                   apiNotesFile);
    llvm::raw_svector_ostream OS(apiNotesBuffer);
    if (api_notes::compileAPINotes(sourceBuffer->getBuffer(),
                                   SourceMgr.getFileEntryForID(sourceFileID),
                                   OS,
                                   srcMgrAdapter.getDiagHandler(),
                                   srcMgrAdapter.getDiagContext()))
      return nullptr;

    // Make a copy of the compiled form into the buffer.
    compiledBuffer = llvm::MemoryBuffer::getMemBufferCopy(
               StringRef(apiNotesBuffer.data(), apiNotesBuffer.size()));
  }

  // Load the binary form we just compiled.
  auto reader = APINotesReader::get(std::move(compiledBuffer), SwiftVersion);
  assert(reader && "Could not load the API notes we just generated?");
  return reader;
}

std::unique_ptr<APINotesReader>
APINotesManager::loadAPINotes(StringRef Buffer) {
  llvm::SmallVector<char, 1024> apiNotesBuffer;
  std::unique_ptr<llvm::MemoryBuffer> compiledBuffer;
  SourceMgrAdapter srcMgrAdapter(
      SourceMgr, SourceMgr.getDiagnostics(), diag::err_apinotes_message,
      diag::warn_apinotes_message, diag::note_apinotes_message, nullptr);
  llvm::raw_svector_ostream OS(apiNotesBuffer);

  if (api_notes::compileAPINotes(Buffer, nullptr, OS,
                                 srcMgrAdapter.getDiagHandler(),
                                 srcMgrAdapter.getDiagContext()))
    return nullptr;

  compiledBuffer = llvm::MemoryBuffer::getMemBufferCopy(
      StringRef(apiNotesBuffer.data(), apiNotesBuffer.size()));
  auto reader = APINotesReader::get(std::move(compiledBuffer), SwiftVersion);
  assert(reader && "Could not load the API notes we just generated?");
  return reader;
}

bool APINotesManager::loadAPINotes(const DirectoryEntry *HeaderDir,
                                   const FileEntry *APINotesFile) {
  assert(Readers.find(HeaderDir) == Readers.end());
  if (auto reader = loadAPINotes(APINotesFile)) {
    Readers[HeaderDir] = reader.release();
    return false;
  }

  Readers[HeaderDir] = nullptr;
  return true;
}

OptionalFileEntryRef
APINotesManager::findAPINotesFile(DirectoryEntryRef directory,
                                  StringRef basename, bool wantPublic) {
  FileManager &fileMgr = SourceMgr.getFileManager();

  llvm::SmallString<128> path;
  path += directory.getName();

  StringRef basenameSuffix = "";
  if (!wantPublic) basenameSuffix = "_private";

  // Look for the source API notes file.
  llvm::sys::path::append(path, 
    llvm::Twine(basename) + basenameSuffix + "." + SOURCE_APINOTES_EXTENSION);
  return fileMgr.getOptionalFileRef(path, /*Open*/true);
}

OptionalDirectoryEntryRef APINotesManager::loadFrameworkAPINotes(
    llvm::StringRef FrameworkPath, llvm::StringRef FrameworkName, bool Public) {
  FileManager &FileMgr = SourceMgr.getFileManager();
  
  llvm::SmallString<128> Path;
  Path += FrameworkPath;
  unsigned FrameworkNameLength = Path.size();

  // Form the path to the APINotes file.
  llvm::sys::path::append(Path, "APINotes");
  if (Public)
    llvm::sys::path::append(Path,
                            (llvm::Twine(FrameworkName) + "."
                              + SOURCE_APINOTES_EXTENSION));
  else
    llvm::sys::path::append(Path,
                            (llvm::Twine(FrameworkName) + "_private."
                              + SOURCE_APINOTES_EXTENSION));

  // Try to open the APINotes file.
  auto APINotesFile = FileMgr.getFile(Path);
  if (!APINotesFile)
    return std::nullopt;

  // Form the path to the corresponding header directory.
  Path.resize(FrameworkNameLength);
  if (Public)
    llvm::sys::path::append(Path, "Headers");
  else
    llvm::sys::path::append(Path, "PrivateHeaders");

  // Try to access the header directory.
  auto HeaderDir = FileMgr.getOptionalDirectoryRef(Path);
  if (!HeaderDir)
    return std::nullopt;

  // Try to load the API notes.
  if (loadAPINotes(*HeaderDir, *APINotesFile))
    return std::nullopt;

  // Success: return the header directory.
  if (Public)
    ++NumPublicFrameworkAPINotes;
  else
    ++NumPrivateFrameworkAPINotes;
  return *HeaderDir;
}

static void checkPrivateAPINotesName(DiagnosticsEngine &diags,
                                     const FileEntry *file,
                                     const Module *module) {
  if (file->tryGetRealPathName().empty())
    return;

  StringRef realFilename =
      llvm::sys::path::filename(file->tryGetRealPathName());
  StringRef realStem = llvm::sys::path::stem(realFilename);
  if (realStem.endswith("_private"))
    return;

  unsigned diagID = diag::warn_apinotes_private_case;
  if (module->IsSystem)
    diagID = diag::warn_apinotes_private_case_system;

  diags.Report(SourceLocation(), diagID) << module->Name << realFilename;
}

/// \returns true if any of \p module's immediate submodules are defined in a
/// private module map
static bool hasPrivateSubmodules(const Module *module) {
  return llvm::any_of(module->submodules(), [](const Module *submodule) {
    return submodule->ModuleMapIsPrivate;
  });
}

llvm::SmallVector<FileEntryRef, 2>
APINotesManager::getCurrentModuleAPINotes(Module *module, bool lookInModule,
                                          ArrayRef<std::string> searchPaths) {
  FileManager &fileMgr = SourceMgr.getFileManager();
  auto moduleName = module->getTopLevelModuleName();
  llvm::SmallVector<FileEntryRef, 2> APINotes;

  // First, look relative to the module itself.
  if (lookInModule) {
    // Local function to try loading an API notes file in the given directory.
    auto tryAPINotes = [&](DirectoryEntryRef dir, bool wantPublic) {
      if (auto file = findAPINotesFile(dir, moduleName, wantPublic)) {
        if (!wantPublic)
          checkPrivateAPINotesName(SourceMgr.getDiagnostics(), *file, module);

        APINotes.push_back(*file);
      }
    };

    if (module->IsFramework) {
      // For frameworks, we search in the "Headers" or "PrivateHeaders"
      // subdirectory.
      //
      // Public modules:
      // - Headers/Foo.apinotes
      // - PrivateHeaders/Foo_private.apinotes (if there are private submodules)
      // Private modules:
      // - PrivateHeaders/Bar.apinotes (except that 'Bar' probably already has
      //   the word "Private" in it in practice)
      llvm::SmallString<128> path;
      path += module->Directory->getName();

      if (!module->ModuleMapIsPrivate) {
        unsigned pathLen = path.size();

        llvm::sys::path::append(path, "Headers");
        if (auto apinotesDir = fileMgr.getOptionalDirectoryRef(path))
          tryAPINotes(*apinotesDir, /*wantPublic=*/true);

        path.resize(pathLen);
      }

      if (module->ModuleMapIsPrivate || hasPrivateSubmodules(module)) {
        llvm::sys::path::append(path, "PrivateHeaders");
        if (auto privateAPINotesDir = fileMgr.getOptionalDirectoryRef(path)) {
          tryAPINotes(*privateAPINotesDir,
                      /*wantPublic=*/module->ModuleMapIsPrivate);
        }
      }
    } else {
      // Public modules:
      // - Foo.apinotes
      // - Foo_private.apinotes (if there are private submodules)
      // Private modules:
      // - Bar.apinotes (except that 'Bar' probably already has the word
      //   "Private" in it in practice)
      tryAPINotes(*module->Directory, /*wantPublic=*/true);
      if (!module->ModuleMapIsPrivate && hasPrivateSubmodules(module))
        tryAPINotes(*module->Directory, /*wantPublic=*/false);
    }

    if (!APINotes.empty())
      return APINotes;
  }

  // Second, look for API notes for this module in the module API
  // notes search paths.
  for (const auto &searchPath : searchPaths) {
    if (auto searchDir = fileMgr.getOptionalDirectoryRef(searchPath)) {
      if (auto file = findAPINotesFile(*searchDir, moduleName)) {
        APINotes.push_back(*file);
        return APINotes;
      }
    }
  }

  // Didn't find any API notes.
  return APINotes;
}


bool APINotesManager::loadCurrentModuleAPINotes(
    Module *module, bool lookInModule, ArrayRef<std::string> searchPaths) {
  assert(!CurrentModuleReaders[0] &&
         "Already loaded API notes for the current module?");

  auto APINotes = getCurrentModuleAPINotes(module, lookInModule, searchPaths);
  unsigned numReaders = 0;
  for (auto file : APINotes) {
    CurrentModuleReaders[numReaders++] = loadAPINotes(file).release();
    if (!getCurrentModuleReaders().empty())
      module->APINotesFile = file.getName().str();
  }

  return numReaders > 0;
}

bool APINotesManager::loadCurrentModuleAPINotesFromBuffer(
    ArrayRef<StringRef> Buffers) {
  unsigned NumReader = 0;
  for (auto Buf : Buffers) {
    auto Reader = loadAPINotes(Buf);
    assert(Reader && "Could not load the API notes we just generated?");

    CurrentModuleReaders[NumReader++] = Reader.release();
  }
  return NumReader;
}

llvm::SmallVector<APINotesReader *, 2> APINotesManager::findAPINotes(SourceLocation Loc) {
  llvm::SmallVector<APINotesReader *, 2> Results;

  // If there are readers for the current module, return them.
  if (!getCurrentModuleReaders().empty()) {
    Results.append(getCurrentModuleReaders().begin(), getCurrentModuleReaders().end());
    return Results;
  }

  // If we're not allowed to implicitly load API notes files, we're done.
  if (!ImplicitAPINotes) return Results;

  // If we don't have source location information, we're done.
  if (Loc.isInvalid()) return Results;

  // API notes are associated with the expansion location. Retrieve the
  // file for this location.
  SourceLocation ExpansionLoc = SourceMgr.getExpansionLoc(Loc);
  FileID ID = SourceMgr.getFileID(ExpansionLoc);
  if (ID.isInvalid()) return Results;
  OptionalFileEntryRef File = SourceMgr.getFileEntryRefForID(ID);
  if (!File) return Results;

  // Look for API notes in the directory corresponding to this file, or one of
  // its its parent directories.
  OptionalDirectoryEntryRef Dir = File->getDir();
  FileManager &FileMgr = SourceMgr.getFileManager();
  llvm::SetVector<const DirectoryEntry *,
                  SmallVector<const DirectoryEntry *, 4>,
                  llvm::SmallPtrSet<const DirectoryEntry *, 4>> DirsVisited;
  do {
    // Look for an API notes reader for this header search directory.
    auto Known = Readers.find(*Dir);

    // If we already know the answer, chase it.
    if (Known != Readers.end()) {
      ++NumDirectoryCacheHits;

      // We've been redirected to another directory for answers. Follow it.
      if (Known->second && Known->second.is<DirectoryEntryRef>()) {
        DirsVisited.insert(*Dir);
        Dir = Known->second.get<DirectoryEntryRef>();
        continue;
      }

      // We have the answer.
      if (auto Reader = Known->second.dyn_cast<APINotesReader *>())
        Results.push_back(Reader);
      break;
    }

    // Look for API notes corresponding to this directory.
    StringRef Path = Dir->getName();
    if (llvm::sys::path::extension(Path) == ".framework") {
      // If this is a framework directory, check whether there are API notes
      // in the APINotes subdirectory.
      auto FrameworkName = llvm::sys::path::stem(Path);
      ++NumFrameworksSearched;

      // Look for API notes for both the public and private headers.
      OptionalDirectoryEntryRef PublicDir =
          loadFrameworkAPINotes(Path, FrameworkName, /*Public=*/true);
      OptionalDirectoryEntryRef PrivateDir =
          loadFrameworkAPINotes(Path, FrameworkName, /*Public=*/false);

      if (PublicDir || PrivateDir) {
        // We found API notes: don't ever look past the framework directory.
        Readers[*Dir] = nullptr;

        // Pretend we found the result in the public or private directory,
        // as appropriate. All headers should be in one of those two places,
        // but be defensive here.
        if (!DirsVisited.empty()) {
          if (PublicDir && DirsVisited.back() == *PublicDir) {
            DirsVisited.pop_back();
            Dir = *PublicDir;
          } else if (PrivateDir && DirsVisited.back() == *PrivateDir) {
            DirsVisited.pop_back();
            Dir = *PrivateDir;
          }
        }

        // Grab the result.
        if (auto Reader = Readers[*Dir].dyn_cast<APINotesReader *>())
          Results.push_back(Reader);
        break;
      }
    } else {
      // Look for an APINotes file in this directory.
      llvm::SmallString<128> APINotesPath;
      APINotesPath += Dir->getName();
      llvm::sys::path::append(APINotesPath,
                              (llvm::Twine("APINotes.")
                                 + SOURCE_APINOTES_EXTENSION));

      // If there is an API notes file here, try to load it.
      ++NumDirectoriesSearched;
      if (auto APINotesFile = FileMgr.getFile(APINotesPath)) {
        if (!loadAPINotes(*Dir, *APINotesFile)) {
          ++NumHeaderAPINotes;
          if (auto Reader = Readers[*Dir].dyn_cast<APINotesReader *>())
            Results.push_back(Reader);
          break;
        }
      }
    }

    // We didn't find anything. Look at the parent directory.
    if (!DirsVisited.insert(*Dir)) {
      Dir = std::nullopt;
      break;
    }

    StringRef ParentPath = llvm::sys::path::parent_path(Path);
    while (llvm::sys::path::stem(ParentPath) == "..") {
      ParentPath = llvm::sys::path::parent_path(ParentPath);
    }
    if (ParentPath.empty()) {
      Dir = std::nullopt;
    } else {
      Dir = FileMgr.getOptionalDirectoryRef(ParentPath);
    }
  } while (Dir);

  // Path compression for all of the directories we visited, redirecting
  // them to the directory we ended on. If no API notes were found, the
  // resulting directory will be NULL, indicating no API notes.
  for (const auto Visited : DirsVisited) {
    Readers[Visited] = Dir ? ReaderEntry(*Dir) : ReaderEntry();
  }

  return Results;
}
