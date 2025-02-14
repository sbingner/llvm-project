//===-- PlatformAppleTVSimulator.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PlatformAppleTVSimulator.h"

#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleList.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/ProcessInfo.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/StreamString.h"

#include "llvm/Support/FileSystem.h"

using namespace lldb;
using namespace lldb_private;

namespace lldb_private {
class Process;
}

// Static Variables
static uint32_t g_initialize_count = 0;

// Static Functions
void PlatformAppleTVSimulator::Initialize() {
  PlatformDarwin::Initialize();

  if (g_initialize_count++ == 0) {
    PluginManager::RegisterPlugin(
        PlatformAppleTVSimulator::GetPluginNameStatic(),
        PlatformAppleTVSimulator::GetDescriptionStatic(),
        PlatformAppleTVSimulator::CreateInstance);
  }
}

void PlatformAppleTVSimulator::Terminate() {
  if (g_initialize_count > 0) {
    if (--g_initialize_count == 0) {
      PluginManager::UnregisterPlugin(PlatformAppleTVSimulator::CreateInstance);
    }
  }

  PlatformDarwin::Terminate();
}

PlatformSP PlatformAppleTVSimulator::CreateInstance(bool force,
                                                    const ArchSpec *arch) {
  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_PLATFORM));
  if (log) {
    const char *arch_name;
    if (arch && arch->GetArchitectureName())
      arch_name = arch->GetArchitectureName();
    else
      arch_name = "<null>";

    const char *triple_cstr =
        arch ? arch->GetTriple().getTriple().c_str() : "<null>";

    LLDB_LOGF(log, "PlatformAppleTVSimulator::%s(force=%s, arch={%s,%s})",
              __FUNCTION__, force ? "true" : "false", arch_name, triple_cstr);
  }

  bool create = force;
  if (!create && arch && arch->IsValid()) {
    switch (arch->GetMachine()) {
    case llvm::Triple::x86_64: {
      const llvm::Triple &triple = arch->GetTriple();
      switch (triple.getVendor()) {
      case llvm::Triple::Apple:
        create = true;
        break;

#if defined(__APPLE__)
      // Only accept "unknown" for the vendor if the host is Apple and it
      // "unknown" wasn't specified (it was just returned because it was NOT
      // specified)
      case llvm::Triple::UnknownVendor:
        create = !arch->TripleVendorWasSpecified();
        break;
#endif
      default:
        break;
      }

      if (create) {
        switch (triple.getOS()) {
        case llvm::Triple::TvOS:
          break;

#if defined(__APPLE__)
        // Only accept "unknown" for the OS if the host is Apple and it
        // "unknown" wasn't specified (it was just returned because it was NOT
        // specified)
        case llvm::Triple::UnknownOS:
          create = !arch->TripleOSWasSpecified();
          break;
#endif
        default:
          create = false;
          break;
        }
      }
    } break;
    default:
      break;
    }
  }
  if (create) {
    LLDB_LOGF(log, "PlatformAppleTVSimulator::%s() creating platform",
              __FUNCTION__);

    return PlatformSP(new PlatformAppleTVSimulator());
  }

  LLDB_LOGF(log, "PlatformAppleTVSimulator::%s() aborting creation of platform",
            __FUNCTION__);

  return PlatformSP();
}

lldb_private::ConstString PlatformAppleTVSimulator::GetPluginNameStatic() {
  static ConstString g_name("tvos-simulator");
  return g_name;
}

const char *PlatformAppleTVSimulator::GetDescriptionStatic() {
  return "Apple TV simulator platform plug-in.";
}

/// Default Constructor
PlatformAppleTVSimulator::PlatformAppleTVSimulator()
    : PlatformDarwin(true), m_sdk_dir_mutex(), m_sdk_directory() {}

/// Destructor.
///
/// The destructor is virtual since this class is designed to be
/// inherited from by the plug-in instance.
PlatformAppleTVSimulator::~PlatformAppleTVSimulator() {}

void PlatformAppleTVSimulator::GetStatus(Stream &strm) {
  Platform::GetStatus(strm);
  const char *sdk_directory = GetSDKDirectoryAsCString();
  if (sdk_directory)
    strm.Printf("  SDK Path: \"%s\"\n", sdk_directory);
  else
    strm.PutCString("  SDK Path: error: unable to locate SDK\n");
}

Status PlatformAppleTVSimulator::ResolveExecutable(
    const ModuleSpec &module_spec, lldb::ModuleSP &exe_module_sp,
    const FileSpecList *module_search_paths_ptr) {
  Status error;
  // Nothing special to do here, just use the actual file and architecture

  ModuleSpec resolved_module_spec(module_spec);

  // If we have "ls" as the exe_file, resolve the executable loation based on
  // the current path variables
  // TODO: resolve bare executables in the Platform SDK
  //    if (!resolved_exe_file.Exists())
  //        resolved_exe_file.ResolveExecutableLocation ();

  // Resolve any executable within a bundle on MacOSX
  // TODO: verify that this handles shallow bundles, if not then implement one
  // ourselves
  Host::ResolveExecutableInBundle(resolved_module_spec.GetFileSpec());

  if (FileSystem::Instance().Exists(resolved_module_spec.GetFileSpec())) {
    if (resolved_module_spec.GetArchitecture().IsValid()) {
      error = ModuleList::GetSharedModule(resolved_module_spec, exe_module_sp,
                                          NULL, NULL, NULL);

      if (exe_module_sp && exe_module_sp->GetObjectFile())
        return error;
      exe_module_sp.reset();
    }
    // No valid architecture was specified or the exact ARM slice wasn't found
    // so ask the platform for the architectures that we should be using (in
    // the correct order) and see if we can find a match that way
    StreamString arch_names;
    ArchSpec platform_arch;
    for (uint32_t idx = 0; GetSupportedArchitectureAtIndex(
             idx, resolved_module_spec.GetArchitecture());
         ++idx) {
      // Only match x86 with x86 and x86_64 with x86_64...
      if (!module_spec.GetArchitecture().IsValid() ||
          module_spec.GetArchitecture().GetCore() ==
              resolved_module_spec.GetArchitecture().GetCore()) {
        error = ModuleList::GetSharedModule(resolved_module_spec, exe_module_sp,
                                            NULL, NULL, NULL);
        // Did we find an executable using one of the
        if (error.Success()) {
          if (exe_module_sp && exe_module_sp->GetObjectFile())
            break;
          else
            error.SetErrorToGenericError();
        }

        if (idx > 0)
          arch_names.PutCString(", ");
        arch_names.PutCString(platform_arch.GetArchitectureName());
      }
    }

    if (error.Fail() || !exe_module_sp) {
      if (FileSystem::Instance().Readable(resolved_module_spec.GetFileSpec())) {
        error.SetErrorStringWithFormat(
            "'%s' doesn't contain any '%s' platform architectures: %s",
            resolved_module_spec.GetFileSpec().GetPath().c_str(),
            GetPluginName().GetCString(), arch_names.GetString().str().c_str());
      } else {
        error.SetErrorStringWithFormat(
            "'%s' is not readable",
            resolved_module_spec.GetFileSpec().GetPath().c_str());
      }
    }
  } else {
    error.SetErrorStringWithFormat("'%s' does not exist",
                                   module_spec.GetFileSpec().GetPath().c_str());
  }

  return error;
}

static FileSystem::EnumerateDirectoryResult
EnumerateDirectoryCallback(void *baton, llvm::sys::fs::file_type ft,
                           llvm::StringRef path) {
  if (ft == llvm::sys::fs::file_type::directory_file) {
    FileSpec file_spec(path);
    const char *filename = file_spec.GetFilename().GetCString();
    if (filename &&
        strncmp(filename, "AppleTVSimulator", strlen("AppleTVSimulator")) ==
            0) {
      ::snprintf((char *)baton, PATH_MAX, "%s", filename);
      return FileSystem::eEnumerateDirectoryResultQuit;
    }
  }
  return FileSystem::eEnumerateDirectoryResultNext;
}

const char *PlatformAppleTVSimulator::GetSDKDirectoryAsCString() {
  std::lock_guard<std::mutex> guard(m_sdk_dir_mutex);
  if (m_sdk_directory.empty()) {
    if (FileSpec fspec = HostInfo::GetXcodeDeveloperDirectory()) {
      std::string developer_dir = fspec.GetPath();
      char sdks_directory[PATH_MAX];
      char sdk_dirname[PATH_MAX];
      sdk_dirname[0] = '\0';
      snprintf(sdks_directory, sizeof(sdks_directory),
               "%s/Platforms/AppleTVSimulator.platform/Developer/SDKs",
               developer_dir.c_str());
      FileSpec simulator_sdk_spec;
      bool find_directories = true;
      bool find_files = false;
      bool find_other = false;
      FileSystem::Instance().EnumerateDirectory(
          sdks_directory, find_directories, find_files, find_other,
          EnumerateDirectoryCallback, sdk_dirname);

      if (sdk_dirname[0]) {
        m_sdk_directory = sdks_directory;
        m_sdk_directory.append(1, '/');
        m_sdk_directory.append(sdk_dirname);
        return m_sdk_directory.c_str();
      }
    }
    // Assign a single NULL character so we know we tried to find the device
    // support directory and we don't keep trying to find it over and over.
    m_sdk_directory.assign(1, '\0');
  }

  // We should have put a single NULL character into m_sdk_directory or it
  // should have a valid path if the code gets here
  assert(m_sdk_directory.empty() == false);
  if (m_sdk_directory[0])
    return m_sdk_directory.c_str();
  return NULL;
}

Status PlatformAppleTVSimulator::GetSymbolFile(const FileSpec &platform_file,
                                               const UUID *uuid_ptr,
                                               FileSpec &local_file) {
  Status error;
  char platform_file_path[PATH_MAX];
  if (platform_file.GetPath(platform_file_path, sizeof(platform_file_path))) {
    char resolved_path[PATH_MAX];

    const char *sdk_dir = GetSDKDirectoryAsCString();
    if (sdk_dir) {
      ::snprintf(resolved_path, sizeof(resolved_path), "%s/%s", sdk_dir,
                 platform_file_path);

      // First try in the SDK and see if the file is in there
      local_file.SetFile(resolved_path, FileSpec::Style::native);
      FileSystem::Instance().Resolve(local_file);
      if (FileSystem::Instance().Exists(local_file))
        return error;

      // Else fall back to the actual path itself
      local_file.SetFile(platform_file_path, FileSpec::Style::native);
      FileSystem::Instance().Resolve(local_file);
      if (FileSystem::Instance().Exists(local_file))
        return error;
    }
    error.SetErrorStringWithFormat(
        "unable to locate a platform file for '%s' in platform '%s'",
        platform_file_path, GetPluginName().GetCString());
  } else {
    error.SetErrorString("invalid platform file argument");
  }
  return error;
}

Status PlatformAppleTVSimulator::GetSharedModule(
    const ModuleSpec &module_spec, lldb_private::Process *process,
    ModuleSP &module_sp, const FileSpecList *module_search_paths_ptr,
    ModuleSP *old_module_sp_ptr, bool *did_create_ptr) {
  // For AppleTV, the SDK files are all cached locally on the host system. So
  // first we ask for the file in the cached SDK, then we attempt to get a
  // shared module for the right architecture with the right UUID.
  Status error;
  ModuleSpec platform_module_spec(module_spec);
  const FileSpec &platform_file = module_spec.GetFileSpec();
  error = GetSymbolFile(platform_file, module_spec.GetUUIDPtr(),
                        platform_module_spec.GetFileSpec());
  if (error.Success()) {
    error = ResolveExecutable(platform_module_spec, module_sp,
                              module_search_paths_ptr);
  } else {
    const bool always_create = false;
    error = ModuleList::GetSharedModule(
        module_spec, module_sp, module_search_paths_ptr, old_module_sp_ptr,
        did_create_ptr, always_create);
  }
  if (module_sp)
    module_sp->SetPlatformFileSpec(platform_file);

  return error;
}

uint32_t PlatformAppleTVSimulator::FindProcesses(
    const ProcessInstanceInfoMatch &match_info,
    ProcessInstanceInfoList &process_infos) {
  ProcessInstanceInfoList all_osx_process_infos;
  // First we get all OSX processes
  const uint32_t n = Host::FindProcesses(match_info, all_osx_process_infos);

  // Now we filter them down to only the TvOS triples
  for (uint32_t i = 0; i < n; ++i) {
    const ProcessInstanceInfo &proc_info = all_osx_process_infos[i];
    if (proc_info.GetArchitecture().GetTriple().getOS() == llvm::Triple::TvOS) {
      process_infos.push_back(proc_info);
    }
  }
  return process_infos.size();
}

bool PlatformAppleTVSimulator::GetSupportedArchitectureAtIndex(uint32_t idx,
                                                               ArchSpec &arch) {
  static const ArchSpec platform_arch(
      HostInfo::GetArchitecture(HostInfo::eArchKind64));

  if (idx == 0) {
    arch = platform_arch;
    if (arch.IsValid()) {
      arch.GetTriple().setOS(llvm::Triple::TvOS);
      return true;
    }
  }
  return false;
}
