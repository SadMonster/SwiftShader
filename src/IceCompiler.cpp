//===- subzero/src/IceCompiler.cpp - Driver for bitcode translation -------===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a driver for translating PNaCl bitcode into native code.
// It can either directly parse the binary bitcode file, or use LLVM routines to
// parse a textual bitcode file into LLVM IR and then convert LLVM IR into ICE.
// In either case, the high-level ICE is then compiled down to native code, as
// either an ELF object file or a textual asm file.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/StreamingMemoryObject.h"

#include "IceCfg.h"
#include "IceClFlags.h"
#include "IceClFlagsExtra.h"
#include "IceCompiler.h"
#include "IceConverter.h"
#include "IceELFObjectWriter.h"
#include "PNaClTranslator.h"
namespace Ice {

namespace {

struct {
  const char *FlagName;
  int FlagValue;
} ConditionalBuildAttributes[] = {{"dump", ALLOW_DUMP},
                                  {"disable_ir_gen", ALLOW_DISABLE_IR_GEN},
                                  {"llvm_cl", ALLOW_LLVM_CL},
                                  {"llvm_ir", ALLOW_LLVM_IR},
                                  {"llvm_ir_as_input", ALLOW_LLVM_IR_AS_INPUT},
                                  {"minimal_build", ALLOW_MINIMAL_BUILD},
                                  {"browser_mode", PNACL_BROWSER_TRANSLATOR}};

// Validates values of build attributes. Prints them to Stream if
// Stream is non-null.
void validateAndGenerateBuildAttributes(Ostream *Stream) {
  // List the supported targets.
  if (Stream) {
#define SUBZERO_TARGET(TARGET) *Stream << "target_" #TARGET << "\n";
#include "llvm/Config/SZTargets.def"
  }

  for (size_t i = 0; i < llvm::array_lengthof(ConditionalBuildAttributes);
       ++i) {
    switch (ConditionalBuildAttributes[i].FlagValue) {
    case 0:
      if (Stream)
        *Stream << "no_" << ConditionalBuildAttributes[i].FlagName << "\n";
      break;
    case 1:
      if (Stream)
        *Stream << "allow_" << ConditionalBuildAttributes[i].FlagName << "\n";
      break;
    default: {
      std::string Buffer;
      llvm::raw_string_ostream StrBuf(Buffer);
      StrBuf << "Flag " << ConditionalBuildAttributes[i].FlagName
             << " must be defined as 0/1. Found: "
             << ConditionalBuildAttributes[i].FlagValue;
      llvm::report_fatal_error(StrBuf.str());
    }
    }
  }
}

} // end of anonymous namespace

void Compiler::run(const Ice::ClFlagsExtra &ExtraFlags, GlobalContext &Ctx,
                   std::unique_ptr<llvm::DataStreamer> &&InputStream) {
  validateAndGenerateBuildAttributes(
      ExtraFlags.getGenerateBuildAtts() ? &Ctx.getStrDump() : nullptr);
  if (ExtraFlags.getGenerateBuildAtts())
    return Ctx.getErrorStatus()->assign(EC_None);

  if (!ALLOW_DISABLE_IR_GEN && Ctx.getFlags().getDisableIRGeneration()) {
    Ctx.getStrDump() << "Error: Build doesn't allow --no-ir-gen when not "
                     << "ALLOW_DISABLE_IR_GEN!\n";
    return Ctx.getErrorStatus()->assign(EC_Args);
  }

  // Force -build-on-read=0 for .ll files.
  const std::string LLSuffix = ".ll";
  const IceString &IRFilename = ExtraFlags.getIRFilename();
  bool BuildOnRead = ExtraFlags.getBuildOnRead();
  if (ALLOW_LLVM_IR_AS_INPUT && IRFilename.length() >= LLSuffix.length() &&
      IRFilename.compare(IRFilename.length() - LLSuffix.length(),
                         LLSuffix.length(), LLSuffix) == 0)
    BuildOnRead = false;

  TimerMarker T(Ice::TimerStack::TT_szmain, &Ctx);

  if (Ctx.getFlags().getOutFileType() == FT_Elf) {
    TimerMarker T1(Ice::TimerStack::TT_emit, &Ctx);
    Ctx.getObjectWriter()->writeInitialELFHeader();
  }

  Ctx.startWorkerThreads();

  std::unique_ptr<Translator> Translator;
  if (BuildOnRead) {
    std::unique_ptr<PNaClTranslator> PTranslator(new PNaClTranslator(&Ctx));
    std::unique_ptr<llvm::StreamingMemoryObject> MemObj(
        new llvm::StreamingMemoryObjectImpl(InputStream.release()));
    PTranslator->translate(IRFilename, std::move(MemObj));
    Translator.reset(PTranslator.release());
  } else if (ALLOW_LLVM_IR) {
    if (PNACL_BROWSER_TRANSLATOR) {
      Ctx.getStrDump()
          << "non BuildOnRead is not supported w/ PNACL_BROWSER_TRANSLATOR\n";
      return Ctx.getErrorStatus()->assign(EC_Args);
    }
    // Parse the input LLVM IR file into a module.
    llvm::SMDiagnostic Err;
    TimerMarker T1(Ice::TimerStack::TT_parse, &Ctx);
    llvm::raw_ostream *Verbose =
        ExtraFlags.getLLVMVerboseErrors() ? &llvm::errs() : nullptr;
    std::unique_ptr<llvm::Module> Mod =
        NaClParseIRFile(IRFilename, ExtraFlags.getInputFileFormat(), Err,
                        Verbose, llvm::getGlobalContext());
    if (!Mod) {
      Err.print(ExtraFlags.getAppName().c_str(), llvm::errs());
      return Ctx.getErrorStatus()->assign(EC_Bitcode);
    }

    std::unique_ptr<Converter> Converter(new class Converter(Mod.get(), &Ctx));
    Converter->convertToIce();
    Translator.reset(Converter.release());
  } else {
    Ctx.getStrDump() << "Error: Build doesn't allow LLVM IR, "
                     << "--build-on-read=0 not allowed\n";
    return Ctx.getErrorStatus()->assign(EC_Args);
  }

  Ctx.waitForWorkerThreads();
  Translator->transferErrorCode();
  Translator->emitConstants();

  if (Ctx.getFlags().getOutFileType() == FT_Elf) {
    TimerMarker T1(Ice::TimerStack::TT_emit, &Ctx);
    Ctx.getObjectWriter()->setUndefinedSyms(Ctx.getConstantExternSyms());
    Ctx.getObjectWriter()->writeNonUserSections();
  }
  if (Ctx.getFlags().getSubzeroTimingEnabled())
    Ctx.dumpTimers();
  if (Ctx.getFlags().getTimeEachFunction()) {
    const bool DumpCumulative = false;
    Ctx.dumpTimers(GlobalContext::TSK_Funcs, DumpCumulative);
  }
  const bool FinalStats = true;
  Ctx.dumpStats("_FINAL_", FinalStats);
}

} // end of namespace Ice
