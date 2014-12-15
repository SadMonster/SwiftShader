//===- subzero/src/PNaClTranslator.cpp - ICE from bitcode -----------------===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the PNaCl bitcode file to Ice, to machine code
// translator.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/Bitcode/NaCl/NaClBitcodeDecoders.h"
#include "llvm/Bitcode/NaCl/NaClBitcodeHeader.h"
#include "llvm/Bitcode/NaCl/NaClBitcodeParser.h"
#include "llvm/Bitcode/NaCl/NaClReaderWriter.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "IceAPInt.h"
#include "IceAPFloat.h"
#include "IceCfg.h"
#include "IceCfgNode.h"
#include "IceClFlags.h"
#include "IceDefs.h"
#include "IceGlobalInits.h"
#include "IceInst.h"
#include "IceOperand.h"
#include "PNaClTranslator.h"

#include <memory>

namespace {
using namespace llvm;

// TODO(kschimpf) Remove error recovery once implementation complete.
static cl::opt<bool> AllowErrorRecovery(
    "allow-pnacl-reader-error-recovery",
    cl::desc("Allow error recovery when reading PNaCl bitcode."),
    cl::init(false));

// Models elements in the list of types defined in the types block.
// These elements can be undefined, a (simple) type, or a function type
// signature. Note that an extended type is undefined on construction.
// Use methods setAsSimpleType and setAsFuncSigType to define
// the extended type.
class ExtendedType {
  // ExtendedType(const ExtendedType &Ty) = delete;
  ExtendedType &operator=(const ExtendedType &Ty) = delete;
public:
  /// Discriminator for LLVM-style RTTI.
  enum TypeKind { Undefined, Simple, FuncSig };

  ExtendedType() : Kind(Undefined) {}

  virtual ~ExtendedType() {}

  ExtendedType::TypeKind getKind() const { return Kind; }
  void dump(Ice::Ostream &Stream) const;

  /// Changes the extended type to a simple type with the given
  /// value.
  void setAsSimpleType(Ice::Type Ty) {
    assert(Kind == Undefined);
    Kind = Simple;
    Signature.setReturnType(Ty);
  }

  /// Changes the extended type to an (empty) function signature type.
  void setAsFunctionType() {
    assert(Kind == Undefined);
    Kind = FuncSig;
  }

protected:
  // Note: For simple types, the return type of the signature will
  // be used to hold the simple type.
  Ice::FuncSigType Signature;

private:
  ExtendedType::TypeKind Kind;
};

Ice::Ostream &operator<<(Ice::Ostream &Stream, const ExtendedType &Ty) {
  if (!ALLOW_DUMP)
    return Stream;
  Ty.dump(Stream);
  return Stream;
}

Ice::Ostream &operator<<(Ice::Ostream &Stream, ExtendedType::TypeKind Kind) {
  if (!ALLOW_DUMP)
    return Stream;
  Stream << "ExtendedType::";
  switch (Kind) {
  case ExtendedType::Undefined:
    Stream << "Undefined";
    break;
  case ExtendedType::Simple:
    Stream << "Simple";
    break;
  case ExtendedType::FuncSig:
    Stream << "FuncSig";
    break;
  default:
    Stream << "??";
    break;
  }
  return Stream;
}

// Models an ICE type as an extended type.
class SimpleExtendedType : public ExtendedType {
  SimpleExtendedType(const SimpleExtendedType &) = delete;
  SimpleExtendedType &operator=(const SimpleExtendedType &) = delete;
public:
  Ice::Type getType() const { return Signature.getReturnType(); }

  static bool classof(const ExtendedType *Ty) {
    return Ty->getKind() == Simple;
  }
};

// Models a function signature as an extended type.
class FuncSigExtendedType : public ExtendedType {
  FuncSigExtendedType(const FuncSigExtendedType &) = delete;
  FuncSigExtendedType &operator=(const FuncSigExtendedType &) = delete;
public:
  const Ice::FuncSigType &getSignature() const { return Signature; }
  void setReturnType(Ice::Type ReturnType) {
    Signature.setReturnType(ReturnType);
  }
  void appendArgType(Ice::Type ArgType) { Signature.appendArgType(ArgType); }
  static bool classof(const ExtendedType *Ty) {
    return Ty->getKind() == FuncSig;
  }
};

void ExtendedType::dump(Ice::Ostream &Stream) const {
  if (!ALLOW_DUMP)
    return;
  Stream << Kind;
  switch (Kind) {
  case Simple: {
    Stream << " " << Signature.getReturnType();
    break;
  }
  case FuncSig: {
    Stream << " " << Signature;
  }
  default:
    break;
  }
}

class BlockParserBaseClass;

// Top-level class to read PNaCl bitcode files, and translate to ICE.
class TopLevelParser : public NaClBitcodeParser {
  TopLevelParser(const TopLevelParser &) = delete;
  TopLevelParser &operator=(const TopLevelParser &) = delete;

public:
  typedef std::vector<Ice::FunctionDeclaration *> FunctionDeclarationListType;

  TopLevelParser(Ice::Translator &Translator, NaClBitcodeHeader &Header,
                 NaClBitstreamCursor &Cursor, bool &ErrorStatus)
      : NaClBitcodeParser(Cursor), Translator(Translator), Header(Header),
        ErrorStatus(ErrorStatus), NumErrors(0), NumFunctionIds(0),
        NumFunctionBlocks(0), BlockParser(nullptr) {
    setErrStream(Translator.getContext()->getStrDump());
  }

  ~TopLevelParser() override {}

  Ice::Translator &getTranslator() { return Translator; }

  void setBlockParser(BlockParserBaseClass *NewBlockParser) {
    BlockParser = NewBlockParser;
  }

  // Generates error with given Message. Always returns true.
  bool Error(const std::string &Message) override;

  // Generates error message with respect to the current block parser.
  bool BlockError(const std::string &Message);

  /// Returns the number of errors found while parsing the bitcode
  /// file.
  unsigned getNumErrors() const { return NumErrors; }

  /// Returns the number of bytes in the bitcode header.
  size_t getHeaderSize() const { return Header.getHeaderSize(); }

  /// Changes the size of the type list to the given size.
  void resizeTypeIDValues(unsigned NewSize) { TypeIDValues.resize(NewSize); }

  /// Returns true if generation of Subzero IR is disabled.
  bool isIRGenerationDisabled() const {
    return ALLOW_DISABLE_IR_GEN ? Translator.getFlags().DisableIRGeneration
                                : false;
  }

  /// Returns the undefined type associated with type ID.
  /// Note: Returns extended type ready to be defined.
  ExtendedType *getTypeByIDForDefining(unsigned ID) {
    // Get corresponding element, verifying the value is still undefined
    // (and hence allowed to be defined).
    ExtendedType *Ty = getTypeByIDAsKind(ID, ExtendedType::Undefined);
    if (Ty)
      return Ty;
    if (ID >= TypeIDValues.size())
      TypeIDValues.resize(ID+1);
    return &TypeIDValues[ID];
  }

  /// Returns the type associated with the given index.
  Ice::Type getSimpleTypeByID(unsigned ID) {
    const ExtendedType *Ty = getTypeByIDAsKind(ID, ExtendedType::Simple);
    if (Ty == nullptr)
      // Return error recovery value.
      return Ice::IceType_void;
    return cast<SimpleExtendedType>(Ty)->getType();
  }

  /// Returns the type signature associated with the given index.
  const Ice::FuncSigType &getFuncSigTypeByID(unsigned ID) {
    const ExtendedType *Ty = getTypeByIDAsKind(ID, ExtendedType::FuncSig);
    if (Ty == nullptr)
      // Return error recovery value.
      return UndefinedFuncSigType;
    return cast<FuncSigExtendedType>(Ty)->getSignature();
  }

  /// Sets the next function ID to the given LLVM function.
  void setNextFunctionID(Ice::FunctionDeclaration *Fcn) {
    ++NumFunctionIds;
    FunctionDeclarationList.push_back(Fcn);
  }

  /// Defines the next function ID as one that has an implementation
  /// (i.e a corresponding function block in the bitcode).
  void setNextValueIDAsImplementedFunction() {
    DefiningFunctionDeclarationsList.push_back(FunctionDeclarationList.size());
  }

  /// Returns the value id that should be associated with the the
  /// current function block. Increments internal counters during call
  /// so that it will be in correct position for next function block.
  unsigned getNextFunctionBlockValueID() {
    if (NumFunctionBlocks >= DefiningFunctionDeclarationsList.size())
      report_fatal_error(
          "More function blocks than defined function addresses");
    return DefiningFunctionDeclarationsList[NumFunctionBlocks++];
  }

  /// Returns the function associated with ID.
  Ice::FunctionDeclaration *getFunctionByID(unsigned ID) {
    if (ID < FunctionDeclarationList.size())
      return FunctionDeclarationList[ID];
    return reportGetFunctionByIDError(ID);
  }

  /// Returns the list of function declarations.
  const FunctionDeclarationListType &getFunctionDeclarationList() const {
    return FunctionDeclarationList;
  }

  /// Returns the corresponding constant associated with a global declaration.
  /// (i.e. relocatable).
  Ice::Constant *getOrCreateGlobalConstantByID(unsigned ID) {
    // TODO(kschimpf): Can this be built when creating global initializers?
    Ice::Constant *C;
    if (ID >= ValueIDConstants.size()) {
      C = nullptr;
      unsigned ExpectedSize =
          FunctionDeclarationList.size() + VariableDeclarations.size();
      if (ID >= ExpectedSize)
        ExpectedSize = ID;
      ValueIDConstants.resize(ExpectedSize);
    } else {
      C = ValueIDConstants[ID];
    }
    if (C != nullptr)
      return C;

    if (isIRGenerationDisabled()) {
      ValueIDConstants[ID] = nullptr;
      return nullptr;
    }

    // If reached, no such constant exists, create one.
    // TODO(kschimpf) Don't get addresses of intrinsic function declarations.
    Ice::GlobalDeclaration *Decl = nullptr;
    unsigned FcnIDSize = FunctionDeclarationList.size();
    if (ID < FcnIDSize) {
      Decl = FunctionDeclarationList[ID];
    } else if ((ID - FcnIDSize) < VariableDeclarations.size()) {
      Decl = VariableDeclarations[ID - FcnIDSize];
    }
    std::string Name;
    bool SuppressMangling;
    if (Decl) {
      Name = Decl->getName();
      SuppressMangling = Decl->getSuppressMangling();
    } else {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Reference to global not defined: " << ID;
      BlockError(StrBuf.str());
      // TODO(kschimpf) Remove error recovery once implementation complete.
      Name = "??";
      SuppressMangling = false;
    }
    const Ice::RelocOffsetT Offset = 0;
    C = getTranslator().getContext()->getConstantSym(Offset, Name,
                                                     SuppressMangling);
    ValueIDConstants[ID] = C;
    return C;
  }

  /// Returns the number of function declarations in the bitcode file.
  unsigned getNumFunctionIDs() const { return NumFunctionIds; }

  /// Returns the number of global declarations (i.e. IDs) defined in
  /// the bitcode file.
  unsigned getNumGlobalIDs() const {
    return FunctionDeclarationList.size() + VariableDeclarations.size();
  }

  /// Creates Count global variable declarations.
  void CreateGlobalVariables(size_t Count) {
    assert(VariableDeclarations.empty());
    Ice::GlobalContext *Context = getTranslator().getContext();
    for (size_t i = 0; i < Count; ++i) {
      VariableDeclarations.push_back(Ice::VariableDeclaration::create(Context));
    }
  }

  /// Returns the number of global variable declarations in the
  /// bitcode file.
  Ice::SizeT getNumGlobalVariables() const {
    return VariableDeclarations.size();
  }

  /// Returns the global variable declaration with the given index.
  Ice::VariableDeclaration *getGlobalVariableByID(unsigned Index) {
    if (Index < VariableDeclarations.size())
      return VariableDeclarations[Index];
    return reportGetGlobalVariableByIDError(Index);
  }

  /// Returns the global declaration (variable or function) with the
  /// given Index.
  Ice::GlobalDeclaration *getGlobalDeclarationByID(size_t Index) {
    if (Index < NumFunctionIds)
      return getFunctionByID(Index);
    else
      return getGlobalVariableByID(Index - NumFunctionIds);
  }

  /// Returns the list of parsed global variable declarations.
  const Ice::Translator::VariableDeclarationListType &getGlobalVariables() {
    return VariableDeclarations;
  }

private:
  // The translator associated with the parser.
  Ice::Translator &Translator;
  // The bitcode header.
  NaClBitcodeHeader &Header;
  // The exit status that should be set to true if an error occurs.
  bool &ErrorStatus;
  // The number of errors reported.
  unsigned NumErrors;
  // The types associated with each type ID.
  std::vector<ExtendedType> TypeIDValues;
  // The set of functions.
  FunctionDeclarationListType FunctionDeclarationList;
  // The set of global variables.
  Ice::Translator::VariableDeclarationListType VariableDeclarations;
  // Relocatable constants associated with global declarations.
  std::vector<Ice::Constant *> ValueIDConstants;
  // The number of function declarations (i.e. IDs).
  unsigned NumFunctionIds;
  // The number of function blocks (processed so far).
  unsigned NumFunctionBlocks;
  // The list of function declaration IDs (in the order found) that
  // aren't just proto declarations.
  // TODO(kschimpf): Instead of using this list, just use
  // FunctionDeclarationList, and the isProto member function.
  std::vector<unsigned> DefiningFunctionDeclarationsList;
  // Error recovery value to use when getFuncSigTypeByID fails.
  Ice::FuncSigType UndefinedFuncSigType;
  // The block parser currently being applied. Used for error
  // reporting.
  BlockParserBaseClass *BlockParser;

  bool ParseBlock(unsigned BlockID) override;

  // Gets extended type associated with the given index, assuming the
  // extended type is of the WantedKind. Generates error message if
  // corresponding extended type of WantedKind can't be found, and
  // returns nullptr.
  ExtendedType *getTypeByIDAsKind(unsigned ID,
                                  ExtendedType::TypeKind WantedKind) {
    ExtendedType *Ty = nullptr;
    if (ID < TypeIDValues.size()) {
      Ty = &TypeIDValues[ID];
      if (Ty->getKind() == WantedKind)
        return Ty;
    }
    // Generate an error message and set ErrorStatus.
    this->reportBadTypeIDAs(ID, Ty, WantedKind);
    return nullptr;
  }

  // Reports that type ID is undefined, or not of the WantedType.
  void reportBadTypeIDAs(unsigned ID, const ExtendedType *Ty,
                         ExtendedType::TypeKind WantedType);

  // Reports that there is no function declaration for ID. Returns an
  // error recovery value to use.
  Ice::FunctionDeclaration *reportGetFunctionByIDError(unsigned ID);

  // Reports that there is not global variable declaration for
  // ID. Returns an error recovery value to use.
  Ice::VariableDeclaration *reportGetGlobalVariableByIDError(unsigned Index);

  // Reports that there is no corresponding ICE type for LLVMTy, and
  // returns ICE::IceType_void.
  Ice::Type convertToIceTypeError(Type *LLVMTy);
};

bool TopLevelParser::Error(const std::string &Message) {
  ErrorStatus = true;
  ++NumErrors;
  NaClBitcodeParser::Error(Message);
  if (!AllowErrorRecovery)
    report_fatal_error("Unable to continue");
  return true;
}

void TopLevelParser::reportBadTypeIDAs(unsigned ID, const ExtendedType *Ty,
                                       ExtendedType::TypeKind WantedType) {
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  if (Ty == nullptr) {
    StrBuf << "Can't find extended type for type id: " << ID;
  } else {
    StrBuf << "Type id " << ID << " not " << WantedType << ". Found: " << *Ty;
  }
  BlockError(StrBuf.str());
}

Ice::FunctionDeclaration *
TopLevelParser::reportGetFunctionByIDError(unsigned ID) {
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "Function index " << ID
         << " not allowed. Out of range. Must be less than "
         << FunctionDeclarationList.size();
  BlockError(StrBuf.str());
  // TODO(kschimpf) Remove error recovery once implementation complete.
  if (!FunctionDeclarationList.empty())
    return FunctionDeclarationList[0];
  report_fatal_error("Unable to continue");
}

Ice::VariableDeclaration *
TopLevelParser::reportGetGlobalVariableByIDError(unsigned Index) {
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "Global index " << Index
         << " not allowed. Out of range. Must be less than "
         << VariableDeclarations.size();
  BlockError(StrBuf.str());
  // TODO(kschimpf) Remove error recovery once implementation complete.
  if (!VariableDeclarations.empty())
    return VariableDeclarations[0];
  report_fatal_error("Unable to continue");
}

Ice::Type TopLevelParser::convertToIceTypeError(Type *LLVMTy) {
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "Invalid LLVM type: " << *LLVMTy;
  Error(StrBuf.str());
  return Ice::IceType_void;
}

// Base class for parsing blocks within the bitcode file.  Note:
// Because this is the base class of block parsers, we generate error
// messages if ParseBlock or ParseRecord is not overridden in derived
// classes.
class BlockParserBaseClass : public NaClBitcodeParser {
  BlockParserBaseClass(const BlockParserBaseClass &) = delete;
  BlockParserBaseClass &operator=(const BlockParserBaseClass &) = delete;

public:
  // Constructor for the top-level module block parser.
  BlockParserBaseClass(unsigned BlockID, TopLevelParser *Context)
      : NaClBitcodeParser(BlockID, Context), Context(Context) {
    Context->setBlockParser(this);
  }

  ~BlockParserBaseClass() override { Context->setBlockParser(nullptr); }

  // Returns the printable name of the type of block being parsed.
  virtual const char *getBlockName() const {
    // If this class is used, it is parsing an unknown block.
    return "unknown";
  }

  // Generates an error Message with the bit address prefixed to it.
  bool Error(const std::string &Message) override;

protected:
  // The context parser that contains the decoded state.
  TopLevelParser *Context;

  // Constructor for nested block parsers.
  BlockParserBaseClass(unsigned BlockID, BlockParserBaseClass *EnclosingParser)
      : NaClBitcodeParser(BlockID, EnclosingParser),
        Context(EnclosingParser->Context) {}

  // Gets the translator associated with the bitcode parser.
  Ice::Translator &getTranslator() const { return Context->getTranslator(); }

  const Ice::ClFlags &getFlags() const { return getTranslator().getFlags(); }

  bool isIRGenerationDisabled() const {
    return ALLOW_DISABLE_IR_GEN ? getTranslator().getFlags().DisableIRGeneration
                                : false;
  }

  // Default implementation. Reports that block is unknown and skips
  // its contents.
  bool ParseBlock(unsigned BlockID) override;

  // Default implementation. Reports that the record is not
  // understood.
  void ProcessRecord() override;

  // Checks if the size of the record is Size.  Return true if valid.
  // Otherwise generates an error and returns false.
  bool isValidRecordSize(unsigned Size, const char *RecordName) {
    const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
    if (Values.size() == Size)
      return true;
    ReportRecordSizeError(Size, RecordName, nullptr);
    return false;
  }

  // Checks if the size of the record is at least as large as the
  // LowerLimit. Returns true if valid.  Otherwise generates an error
  // and returns false.
  bool isValidRecordSizeAtLeast(unsigned LowerLimit, const char *RecordName) {
    const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
    if (Values.size() >= LowerLimit)
      return true;
    ReportRecordSizeError(LowerLimit, RecordName, "at least");
    return false;
  }

  // Checks if the size of the record is no larger than the
  // UpperLimit.  Returns true if valid.  Otherwise generates an error
  // and returns false.
  bool isValidRecordSizeAtMost(unsigned UpperLimit, const char *RecordName) {
    const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
    if (Values.size() <= UpperLimit)
      return true;
    ReportRecordSizeError(UpperLimit, RecordName, "no more than");
    return false;
  }

  // Checks if the size of the record is at least as large as the
  // LowerLimit, and no larger than the UpperLimit.  Returns true if
  // valid.  Otherwise generates an error and returns false.
  bool isValidRecordSizeInRange(unsigned LowerLimit, unsigned UpperLimit,
                                const char *RecordName) {
    return isValidRecordSizeAtLeast(LowerLimit, RecordName) ||
           isValidRecordSizeAtMost(UpperLimit, RecordName);
  }

private:
  /// Generates a record size error. ExpectedSize is the number
  /// of elements expected. RecordName is the name of the kind of
  /// record that has incorrect size. ContextMessage (if not nullptr)
  /// is appended to "record expects" to describe how ExpectedSize
  /// should be interpreted.
  void ReportRecordSizeError(unsigned ExpectedSize, const char *RecordName,
                             const char *ContextMessage);
};

bool TopLevelParser::BlockError(const std::string &Message) {
  if (BlockParser)
    return BlockParser->Error(Message);
  else
    return Error(Message);
}

// Generates an error Message with the bit address prefixed to it.
bool BlockParserBaseClass::Error(const std::string &Message) {
  uint64_t Bit = Record.GetStartBit() + Context->getHeaderSize() * 8;
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "(" << format("%" PRIu64 ":%u", (Bit / 8),
                          static_cast<unsigned>(Bit % 8)) << ") ";
  // Note: If dump routines have been turned off, the error messages
  // will not be readable. Hence, replace with simple error.
  if (ALLOW_DUMP)
    StrBuf << Message;
  else {
    StrBuf << "Invalid " << getBlockName() << " record: <" << Record.GetCode();
    for (const uint64_t Val : Record.GetValues()) {
      StrBuf << " " << Val;
    }
    StrBuf << ">";
  }
  return Context->Error(StrBuf.str());
}

void BlockParserBaseClass::ReportRecordSizeError(unsigned ExpectedSize,
                                                 const char *RecordName,
                                                 const char *ContextMessage) {
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  const char *BlockName = getBlockName();
  const char FirstChar = toupper(*BlockName);
  StrBuf << FirstChar << (BlockName + 1) << " " << RecordName
         << " record expects";
  if (ContextMessage)
    StrBuf << " " << ContextMessage;
  StrBuf << " " << ExpectedSize << " argument";
  if (ExpectedSize > 1)
    StrBuf << "s";
  StrBuf << ". Found: " << Record.GetValues().size();
  Error(StrBuf.str());
}

bool BlockParserBaseClass::ParseBlock(unsigned BlockID) {
  // If called, derived class doesn't know how to handle block.
  // Report error and skip.
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "Don't know how to parse block id: " << BlockID;
  Error(StrBuf.str());
  // TODO(kschimpf) Remove error recovery once implementation complete.
  SkipBlock();
  return false;
}

void BlockParserBaseClass::ProcessRecord() {
  // If called, derived class doesn't know how to handle.
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "Don't know how to process " << getBlockName()
         << " record:" << Record;
  Error(StrBuf.str());
}

// Class to parse a types block.
class TypesParser : public BlockParserBaseClass {
public:
  TypesParser(unsigned BlockID, BlockParserBaseClass *EnclosingParser)
      : BlockParserBaseClass(BlockID, EnclosingParser),
        Timer(Ice::TimerStack::TT_parseTypes, getTranslator().getContext()),
        NextTypeId(0) {}

  ~TypesParser() override {}

private:
  Ice::TimerMarker Timer;
  // The type ID that will be associated with the next type defining
  // record in the types block.
  unsigned NextTypeId;

  void ProcessRecord() override;

  const char *getBlockName() const override { return "type"; }

  void setNextTypeIDAsSimpleType(Ice::Type Ty) {
    Context->getTypeByIDForDefining(NextTypeId++)->setAsSimpleType(Ty);
  }
};

void TypesParser::ProcessRecord() {
  const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
  switch (Record.GetCode()) {
  case naclbitc::TYPE_CODE_NUMENTRY:
    // NUMENTRY: [numentries]
    if (!isValidRecordSize(1, "count"))
      return;
    Context->resizeTypeIDValues(Values[0]);
    return;
  case naclbitc::TYPE_CODE_VOID:
    // VOID
    if (!isValidRecordSize(0, "void"))
      return;
    setNextTypeIDAsSimpleType(Ice::IceType_void);
    return;
  case naclbitc::TYPE_CODE_FLOAT:
    // FLOAT
    if (!isValidRecordSize(0, "float"))
      return;
    setNextTypeIDAsSimpleType(Ice::IceType_f32);
    return;
  case naclbitc::TYPE_CODE_DOUBLE:
    // DOUBLE
    if (!isValidRecordSize(0, "double"))
      return;
    setNextTypeIDAsSimpleType(Ice::IceType_f64);
    return;
  case naclbitc::TYPE_CODE_INTEGER:
    // INTEGER: [width]
    if (!isValidRecordSize(1, "integer"))
      return;
    switch (Values[0]) {
    case 1:
      setNextTypeIDAsSimpleType(Ice::IceType_i1);
      return;
    case 8:
      setNextTypeIDAsSimpleType(Ice::IceType_i8);
      return;
    case 16:
      setNextTypeIDAsSimpleType(Ice::IceType_i16);
      return;
    case 32:
      setNextTypeIDAsSimpleType(Ice::IceType_i32);
      return;
    case 64:
      setNextTypeIDAsSimpleType(Ice::IceType_i64);
      return;
    default:
      break;
    }
    {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Type integer record with invalid bitsize: " << Values[0];
      Error(StrBuf.str());
    }
    return;
  case naclbitc::TYPE_CODE_VECTOR: {
    // VECTOR: [numelts, eltty]
    if (!isValidRecordSize(2, "vector"))
      return;
    Ice::Type BaseTy = Context->getSimpleTypeByID(Values[1]);
    Ice::SizeT Size = Values[0];
    switch (BaseTy) {
    case Ice::IceType_i1:
      switch (Size) {
      case 4:
        setNextTypeIDAsSimpleType(Ice::IceType_v4i1);
        return;
      case 8:
        setNextTypeIDAsSimpleType(Ice::IceType_v8i1);
        return;
      case 16:
        setNextTypeIDAsSimpleType(Ice::IceType_v16i1);
        return;
      default:
        break;
      }
      break;
    case Ice::IceType_i8:
      if (Size == 16) {
        setNextTypeIDAsSimpleType(Ice::IceType_v16i8);
        return;
      }
      break;
    case Ice::IceType_i16:
      if (Size == 8) {
        setNextTypeIDAsSimpleType(Ice::IceType_v8i16);
        return;
      }
      break;
    case Ice::IceType_i32:
      if (Size == 4) {
        setNextTypeIDAsSimpleType(Ice::IceType_v4i32);
        return;
      }
      break;
    case Ice::IceType_f32:
      if (Size == 4) {
        setNextTypeIDAsSimpleType(Ice::IceType_v4f32);
        return;
      }
      break;
    default:
      break;
    }
    {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Invalid type vector record: <" << Values[0] << " x " << BaseTy
             << ">";
      Error(StrBuf.str());
    }
    return;
  }
  case naclbitc::TYPE_CODE_FUNCTION: {
    // FUNCTION: [vararg, retty, paramty x N]
    if (!isValidRecordSizeAtLeast(2, "signature"))
      return;
    if (Values[0])
      Error("Function type can't define varargs");
    ExtendedType *Ty = Context->getTypeByIDForDefining(NextTypeId++);
    Ty->setAsFunctionType();
    FuncSigExtendedType *FuncTy = cast<FuncSigExtendedType>(Ty);
    FuncTy->setReturnType(Context->getSimpleTypeByID(Values[1]));
    for (unsigned i = 2, e = Values.size(); i != e; ++i) {
      // Check that type void not used as argument type.
      // Note: PNaCl restrictions can't be checked until we
      // know the name, because we have to check for intrinsic signatures.
      Ice::Type ArgTy = Context->getSimpleTypeByID(Values[i]);
      if (ArgTy == Ice::IceType_void) {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Type for parameter " << (i - 1)
               << " not valid. Found: " << ArgTy;
        // TODO(kschimpf) Remove error recovery once implementation complete.
        ArgTy = Ice::IceType_i32;
      }
      FuncTy->appendArgType(ArgTy);
    }
    return;
  }
  default:
    BlockParserBaseClass::ProcessRecord();
    return;
  }
  llvm_unreachable("Unknown type block record not processed!");
}

/// Parses the globals block (i.e. global variable declarations and
/// corresponding initializers).
class GlobalsParser : public BlockParserBaseClass {
public:
  GlobalsParser(unsigned BlockID, BlockParserBaseClass *EnclosingParser)
      : BlockParserBaseClass(BlockID, EnclosingParser),
        Timer(Ice::TimerStack::TT_parseGlobals, getTranslator().getContext()),
        InitializersNeeded(0), NextGlobalID(0),
        DummyGlobalVar(
            Ice::VariableDeclaration::create(getTranslator().getContext())),
        CurGlobalVar(DummyGlobalVar) {}

  ~GlobalsParser() final {}

  const char *getBlockName() const override { return "globals"; }

private:
  Ice::TimerMarker Timer;
  // Keeps track of how many initializers are expected for the global variable
  // declaration being built.
  unsigned InitializersNeeded;

  // The index of the next global variable declaration.
  unsigned NextGlobalID;

  // Dummy global variable declaration to guarantee CurGlobalVar is
  // always defined (allowing code to not need to check if
  // CurGlobalVar is nullptr).
  Ice::VariableDeclaration *DummyGlobalVar;

  // Holds the current global variable declaration being built.
  Ice::VariableDeclaration *CurGlobalVar;

  void ExitBlock() override {
    verifyNoMissingInitializers();
    unsigned NumIDs = Context->getNumGlobalVariables();
    if (NextGlobalID < NumIDs) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << getBlockName() << " block expects " << NumIDs
             << " global variable declarations. Found: " << NextGlobalID;
      Error(StrBuf.str());
    }
    BlockParserBaseClass::ExitBlock();
  }

  void ProcessRecord() override;

  // Checks if the number of initializers for the CurGlobalVar is the same as
  // the number found in the bitcode file. If different, and error message is
  // generated, and the internal state of the parser is fixed so this condition
  // is no longer violated.
  void verifyNoMissingInitializers() {
    size_t NumInits = CurGlobalVar->getInitializers().size();
    if (InitializersNeeded != NumInits) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Global variable @g" << NextGlobalID << " expected "
             << InitializersNeeded << " initializer";
      if (InitializersNeeded > 1)
        StrBuf << "s";
      StrBuf << ". Found: " << NumInits;
      Error(StrBuf.str());
      InitializersNeeded = NumInits;
    }
  }
};

void GlobalsParser::ProcessRecord() {
  const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
  switch (Record.GetCode()) {
  case naclbitc::GLOBALVAR_COUNT:
    // COUNT: [n]
    if (!isValidRecordSize(1, "count"))
      return;
    if (NextGlobalID != Context->getNumGlobalVariables()) {
      Error("Globals count record not first in block.");
      return;
    }
    Context->CreateGlobalVariables(Values[0]);
    return;
  case naclbitc::GLOBALVAR_VAR: {
    // VAR: [align, isconst]
    if (!isValidRecordSize(2, "variable"))
      return;
    verifyNoMissingInitializers();
    if (!isIRGenerationDisabled()) {
      InitializersNeeded = 1;
      CurGlobalVar = Context->getGlobalVariableByID(NextGlobalID);
      CurGlobalVar->setAlignment((1 << Values[0]) >> 1);
      CurGlobalVar->setIsConstant(Values[1] != 0);
    }
    ++NextGlobalID;
    return;
  }
  case naclbitc::GLOBALVAR_COMPOUND:
    // COMPOUND: [size]
    if (!isValidRecordSize(1, "compound"))
      return;
    if (!CurGlobalVar->getInitializers().empty()) {
      Error("Globals compound record not first initializer");
      return;
    }
    if (Values[0] < 2) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << getBlockName()
             << " compound record size invalid. Found: " << Values[0];
      Error(StrBuf.str());
      return;
    }
    if (isIRGenerationDisabled())
      return;
    InitializersNeeded = Values[0];
    return;
  case naclbitc::GLOBALVAR_ZEROFILL: {
    // ZEROFILL: [size]
    if (!isValidRecordSize(1, "zerofill"))
      return;
    if (isIRGenerationDisabled())
      return;
    CurGlobalVar->addInitializer(
        new Ice::VariableDeclaration::ZeroInitializer(Values[0]));
    return;
  }
  case naclbitc::GLOBALVAR_DATA: {
    // DATA: [b0, b1, ...]
    if (!isValidRecordSizeAtLeast(1, "data"))
      return;
    if (isIRGenerationDisabled())
      return;
    CurGlobalVar->addInitializer(
        new Ice::VariableDeclaration::DataInitializer(Values));
    return;
  }
  case naclbitc::GLOBALVAR_RELOC: {
    // RELOC: [val, [addend]]
    if (!isValidRecordSizeInRange(1, 2, "reloc"))
      return;
    if (isIRGenerationDisabled())
      return;
    unsigned Index = Values[0];
    Ice::SizeT Offset = 0;
    if (Values.size() == 2)
      Offset = Values[1];
    CurGlobalVar->addInitializer(new Ice::VariableDeclaration::RelocInitializer(
        Context->getGlobalDeclarationByID(Index), Offset));
    return;
  }
  default:
    BlockParserBaseClass::ProcessRecord();
    return;
  }
}

/// Base class for parsing a valuesymtab block in the bitcode file.
class ValuesymtabParser : public BlockParserBaseClass {
  ValuesymtabParser(const ValuesymtabParser &) = delete;
  void operator=(const ValuesymtabParser &) = delete;

public:
  ValuesymtabParser(unsigned BlockID, BlockParserBaseClass *EnclosingParser)
      : BlockParserBaseClass(BlockID, EnclosingParser) {}

  ~ValuesymtabParser() override {}

  const char *getBlockName() const override { return "valuesymtab"; }

protected:
  typedef SmallString<128> StringType;

  // Associates Name with the value defined by the given Index.
  virtual void setValueName(uint64_t Index, StringType &Name) = 0;

  // Associates Name with the value defined by the given Index;
  virtual void setBbName(uint64_t Index, StringType &Name) = 0;

private:

  void ProcessRecord() override;

  void ConvertToString(StringType &ConvertedName) {
    const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
    for (size_t i = 1, e = Values.size(); i != e; ++i) {
      ConvertedName += static_cast<char>(Values[i]);
    }
  }
};

void ValuesymtabParser::ProcessRecord() {
  const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
  StringType ConvertedName;
  switch (Record.GetCode()) {
  case naclbitc::VST_CODE_ENTRY: {
    // VST_ENTRY: [ValueId, namechar x N]
    if (!isValidRecordSizeAtLeast(2, "value entry"))
      return;
    ConvertToString(ConvertedName);
    setValueName(Values[0], ConvertedName);
    return;
  }
  case naclbitc::VST_CODE_BBENTRY: {
    // VST_BBENTRY: [BbId, namechar x N]
    if (!isValidRecordSizeAtLeast(2, "basic block entry"))
      return;
    ConvertToString(ConvertedName);
    setBbName(Values[0], ConvertedName);
    return;
  }
  default:
    break;
  }
  // If reached, don't know how to handle record.
  BlockParserBaseClass::ProcessRecord();
  return;
}

/// Parses function blocks in the bitcode file.
class FunctionParser : public BlockParserBaseClass {
  FunctionParser(const FunctionParser &) = delete;
  FunctionParser &operator=(const FunctionParser &) = delete;

public:
  FunctionParser(unsigned BlockID, BlockParserBaseClass *EnclosingParser)
      : BlockParserBaseClass(BlockID, EnclosingParser),
        Timer(Ice::TimerStack::TT_parseFunctions, getTranslator().getContext()),
        Func(isIRGenerationDisabled()
                 ? nullptr
                 : new Ice::Cfg(getTranslator().getContext())),
        CurrentBbIndex(0), FcnId(Context->getNextFunctionBlockValueID()),
        FuncDecl(Context->getFunctionByID(FcnId)),
        CachedNumGlobalValueIDs(Context->getNumGlobalIDs()),
        NextLocalInstIndex(Context->getNumGlobalIDs()),
        InstIsTerminating(false) {
    if (ALLOW_DUMP && getFlags().TimeEachFunction)
      getTranslator().getContext()->pushTimer(
          getTranslator().getContext()->getTimerID(
              Ice::GlobalContext::TSK_Funcs, FuncDecl->getName()),
          Ice::GlobalContext::TSK_Funcs);
    // TODO(kschimpf) Clean up API to add a function signature to
    // a CFG.
    const Ice::FuncSigType &Signature = FuncDecl->getSignature();
    if (isIRGenerationDisabled()) {
      CurrentNode = nullptr;
      for (Ice::Type ArgType : Signature.getArgList()) {
        (void)ArgType;
        setNextLocalInstIndex(nullptr);
      }
    } else {
      Func->setFunctionName(FuncDecl->getName());
      Func->setReturnType(Signature.getReturnType());
      Func->setInternal(FuncDecl->getLinkage() == GlobalValue::InternalLinkage);
      CurrentNode = InstallNextBasicBlock();
      Func->setEntryNode(CurrentNode);
      for (Ice::Type ArgType : Signature.getArgList()) {
        Func->addArg(getNextInstVar(ArgType));
      }
    }
  }

  ~FunctionParser() final {}

  const char *getBlockName() const override { return "function"; }

  Ice::Cfg *getFunc() const {
    return Func;
  }

  uint32_t getNumGlobalIDs() const {
    return CachedNumGlobalValueIDs;
  }

  void setNextLocalInstIndex(Ice::Operand *Op) {
    setOperand(NextLocalInstIndex++, Op);
  }

  // Set the next constant ID to the given constant C.
  void setNextConstantID(Ice::Constant *C) { setNextLocalInstIndex(C); }

  // Returns the value referenced by the given value Index.
  Ice::Operand *getOperand(uint32_t Index) {
    if (Index < CachedNumGlobalValueIDs) {
      return Context->getOrCreateGlobalConstantByID(Index);
    }
    uint32_t LocalIndex = Index - CachedNumGlobalValueIDs;
    if (LocalIndex >= LocalOperands.size()) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Value index " << Index << " not defined!";
      Error(StrBuf.str());
      report_fatal_error("Unable to continue");
    }
    Ice::Operand *Op = LocalOperands[LocalIndex];
    if (Op == nullptr) {
      if (isIRGenerationDisabled())
        return nullptr;
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Value index " << Index << " not defined!";
      Error(StrBuf.str());
      report_fatal_error("Unable to continue");
    }
    return Op;
  }

private:
  Ice::TimerMarker Timer;
  // The corresponding ICE function defined by the function block.
  Ice::Cfg *Func;
  // The index to the current basic block being built.
  uint32_t CurrentBbIndex;
  // The basic block being built.
  Ice::CfgNode *CurrentNode;
  // The ID for the function.
  unsigned FcnId;
  // The corresponding function declaration.
  Ice::FunctionDeclaration *FuncDecl;
  // Holds the dividing point between local and global absolute value indices.
  uint32_t CachedNumGlobalValueIDs;
  // Holds operands local to the function block, based on indices
  // defined in the bitcode file.
  std::vector<Ice::Operand *> LocalOperands;
  // Holds the index within LocalOperands corresponding to the next
  // instruction that generates a value.
  uint32_t NextLocalInstIndex;
  // True if the last processed instruction was a terminating
  // instruction.
  bool InstIsTerminating;
  // Upper limit of alignment power allowed by LLVM
  static const uint32_t AlignPowerLimit = 29;

  void popTimerIfTimingEachFunction() const {
    if (ALLOW_DUMP && getFlags().TimeEachFunction) {
      getTranslator().getContext()->popTimer(
          getTranslator().getContext()->getTimerID(
              Ice::GlobalContext::TSK_Funcs, Func->getFunctionName()),
          Ice::GlobalContext::TSK_Funcs);
    }
  }

  // Extracts the corresponding Alignment to use, given the AlignPower
  // (i.e. 2**AlignPower, or 0 if AlignPower == 0). InstName is the
  // name of the instruction the alignment appears in.
  void extractAlignment(const char *InstName, uint32_t AlignPower,
                        uint32_t &Alignment) {
    if (AlignPower <= AlignPowerLimit) {
      Alignment = (1 << AlignPower) >> 1;
      return;
    }
    std::string Buffer;
    raw_string_ostream StrBuf(Buffer);
    StrBuf << InstName << " alignment greater than 2**" << AlignPowerLimit
           << ". Found: 2**" << AlignPower;
    Error(StrBuf.str());
    // Error recover with value that is always acceptable.
    Alignment = 1;
  }

  bool ParseBlock(unsigned BlockID) override;

  void ProcessRecord() override;

  void ExitBlock() override;

  // Creates and appends a new basic block to the list of basic blocks.
  Ice::CfgNode *InstallNextBasicBlock() {
    assert(!isIRGenerationDisabled());
    return Func->makeNode();
  }

  // Returns the Index-th basic block in the list of basic blocks.
  Ice::CfgNode *getBasicBlock(uint32_t Index) {
    assert(!isIRGenerationDisabled());
    const Ice::NodeList &Nodes = Func->getNodes();
    if (Index >= Nodes.size()) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Reference to basic block " << Index
             << " not found. Must be less than " << Nodes.size();
      Error(StrBuf.str());
      // TODO(kschimpf) Remove error recovery once implementation complete.
      Index = 0;
    }
    return Nodes[Index];
  }

  // Returns the Index-th basic block in the list of basic blocks.
  // Assumes Index corresponds to a branch instruction. Hence, if
  // the branch references the entry block, it also generates a
  // corresponding error.
  Ice::CfgNode *getBranchBasicBlock(uint32_t Index) {
    assert(!isIRGenerationDisabled());
    if (Index == 0) {
      Error("Branch to entry block not allowed");
      // TODO(kschimpf) Remove error recovery once implementation complete.
    }
    return getBasicBlock(Index);
  }

  // Generate an instruction variable with type Ty.
  Ice::Variable *createInstVar(Ice::Type Ty) {
    assert(!isIRGenerationDisabled());
    if (Ty == Ice::IceType_void) {
      Error("Can't define instruction value using type void");
      // Recover since we can't throw an exception.
      Ty = Ice::IceType_i32;
    }
    return Func->makeVariable(Ty);
  }

  // Generates the next available local variable using the given type.
  Ice::Variable *getNextInstVar(Ice::Type Ty) {
    assert(!isIRGenerationDisabled());
    assert(NextLocalInstIndex >= CachedNumGlobalValueIDs);
    // Before creating one, see if a forwardtyperef has already defined it.
    uint32_t LocalIndex = NextLocalInstIndex - CachedNumGlobalValueIDs;
    if (LocalIndex < LocalOperands.size()) {
      Ice::Operand *Op = LocalOperands[LocalIndex];
      if (Op != nullptr) {
        if (Ice::Variable *Var = dyn_cast<Ice::Variable>(Op)) {
          if (Var->getType() == Ty) {
            ++NextLocalInstIndex;
            return Var;
          }
        }
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Illegal forward referenced instruction ("
               << NextLocalInstIndex << "): " << *Op;
        Error(StrBuf.str());
        // TODO(kschimpf) Remove error recovery once implementation complete.
        ++NextLocalInstIndex;
        return createInstVar(Ty);
      }
    }
    Ice::Variable *Var = createInstVar(Ty);
    setOperand(NextLocalInstIndex++, Var);
    return Var;
  }

  // Converts a relative index (wrt to BaseIndex) to an absolute value
  // index.
  uint32_t convertRelativeToAbsIndex(int32_t Id, int32_t BaseIndex) {
    if (BaseIndex < Id) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Invalid relative value id: " << Id
             << " (must be <= " << BaseIndex << ")";
      Error(StrBuf.str());
      // TODO(kschimpf) Remove error recovery once implementation complete.
      return 0;
    }
    return BaseIndex - Id;
  }

  // Sets element Index (in the local operands list) to Op.
  void setOperand(uint32_t Index, Ice::Operand *Op) {
    assert(Op || isIRGenerationDisabled());
    // Check if simple push works.
    uint32_t LocalIndex = Index - CachedNumGlobalValueIDs;
    if (LocalIndex == LocalOperands.size()) {
      LocalOperands.push_back(Op);
      return;
    }

    // Must be forward reference, expand vector to accommodate.
    if (LocalIndex >= LocalOperands.size())
      LocalOperands.resize(LocalIndex + 1);

    // If element not defined, set it.
    Ice::Operand *OldOp = LocalOperands[LocalIndex];
    if (OldOp == nullptr) {
      LocalOperands[LocalIndex] = Op;
      return;
    }

    // See if forward reference matches.
    if (OldOp == Op)
      return;

    // Error has occurred.
    std::string Buffer;
    raw_string_ostream StrBuf(Buffer);
    StrBuf << "Multiple definitions for index " << Index << ": " << *Op
           << " and " << *OldOp;
    Error(StrBuf.str());
    // TODO(kschimpf) Remove error recovery once implementation complete.
    LocalOperands[LocalIndex] = Op;
  }

  // Returns the relative operand (wrt to BaseIndex) referenced by
  // the given value Index.
  Ice::Operand *getRelativeOperand(int32_t Index, int32_t BaseIndex) {
    return getOperand(convertRelativeToAbsIndex(Index, BaseIndex));
  }

  // Returns the absolute index of the next value generating instruction.
  uint32_t getNextInstIndex() const { return NextLocalInstIndex; }

  // Generates type error message for binary operator Op
  // operating on Type OpTy.
  void ReportInvalidBinaryOp(Ice::InstArithmetic::OpKind Op, Ice::Type OpTy);

  // Validates if integer logical Op, for type OpTy, is valid.
  // Returns true if valid. Otherwise generates error message and
  // returns false.
  bool isValidIntegerLogicalOp(Ice::InstArithmetic::OpKind Op, Ice::Type OpTy) {
    if (Ice::isIntegerType(OpTy))
      return true;
    ReportInvalidBinaryOp(Op, OpTy);
    return false;
  }

  // Validates if integer (or vector of integers) arithmetic Op, for type
  // OpTy, is valid.  Returns true if valid. Otherwise generates
  // error message and returns false.
  bool isValidIntegerArithOp(Ice::InstArithmetic::OpKind Op, Ice::Type OpTy) {
    if (Ice::isIntegerArithmeticType(OpTy))
      return true;
    ReportInvalidBinaryOp(Op, OpTy);
    return false;
  }

  // Checks if floating arithmetic Op, for type OpTy, is valid.
  // Returns true if valid. Otherwise generates an error message and
  // returns false;
  bool isValidFloatingArithOp(Ice::InstArithmetic::OpKind Op, Ice::Type OpTy) {
    if (Ice::isFloatingType(OpTy))
      return true;
    ReportInvalidBinaryOp(Op, OpTy);
    return false;
  }

  // Checks if the type of operand Op is the valid pointer type, for
  // the given InstructionName. Returns true if valid. Otherwise
  // generates an error message and returns false.
  bool isValidPointerType(Ice::Operand *Op, const char *InstructionName) {
    Ice::Type PtrType = Ice::getPointerType();
    if (Op->getType() == PtrType)
      return true;
    std::string Buffer;
    raw_string_ostream StrBuf(Buffer);
    StrBuf << InstructionName << " address not " << PtrType
           << ". Found: " << *Op;
    Error(StrBuf.str());
    return false;
  }

  // Checks if loading/storing a value of type Ty is allowed.
  // Returns true if Valid. Otherwise generates an error message and
  // returns false.
  bool isValidLoadStoreType(Ice::Type Ty, const char *InstructionName) {
    if (isLoadStoreType(Ty))
      return true;
    std::string Buffer;
    raw_string_ostream StrBuf(Buffer);
    StrBuf << InstructionName << " type not allowed: " << Ty << "*";
    Error(StrBuf.str());
    return false;
  }

  // Checks if loading/storing a value of type Ty is allowed for
  // the given Alignment. Otherwise generates an error message and
  // returns false.
  bool isValidLoadStoreAlignment(size_t Alignment, Ice::Type Ty,
                                 const char *InstructionName) {
    if (!isValidLoadStoreType(Ty, InstructionName))
      return false;
    if (isAllowedAlignment(Alignment, Ty))
      return true;
    std::string Buffer;
    raw_string_ostream StrBuf(Buffer);
    StrBuf << InstructionName << " " << Ty << "*: not allowed for alignment "
           << Alignment;
    Error(StrBuf.str());
    return false;
  }

  // Defines if the given alignment is valid for the given type. Simplified
  // version of PNaClABIProps::isAllowedAlignment, based on API's offered
  // for Ice::Type.
  bool isAllowedAlignment(size_t Alignment, Ice::Type Ty) const {
    return Alignment == typeAlignInBytes(Ty) ||
           (Alignment == 1 && !isVectorType(Ty));
  }

  // Types of errors that can occur for insertelement and extractelement
  // instructions.
  enum VectorIndexCheckValue {
    VectorIndexNotVector,
    VectorIndexNotConstant,
    VectorIndexNotInRange,
    VectorIndexNotI32,
    VectorIndexValid
  };

  void dumpVectorIndexCheckValue(raw_ostream &Stream,
                                 VectorIndexCheckValue Value) const {
    if (!ALLOW_DUMP)
      return;
    switch (Value) {
    default:
      report_fatal_error("Unknown VectorIndexCheckValue");
      break;
    case VectorIndexNotVector:
      Stream << "Vector index on non vector";
      break;
    case VectorIndexNotConstant:
      Stream << "Vector index not integer constant";
      break;
    case VectorIndexNotInRange:
      Stream << "Vector index not in range of vector";
      break;
    case VectorIndexNotI32:
      Stream << "Vector index not of type " << Ice::IceType_i32;
      break;
    case VectorIndexValid:
      Stream << "Valid vector index";
      break;
    }
  }

  // Returns whether the given vector index (for insertelement and
  // extractelement instructions) is valid.
  VectorIndexCheckValue validateVectorIndex(const Ice::Operand *Vec,
                                            const Ice::Operand *Index) const {
    Ice::Type VecType = Vec->getType();
    if (!Ice::isVectorType(VecType))
      return VectorIndexNotVector;
    const auto *C = dyn_cast<Ice::ConstantInteger32>(Index);
    if (C == nullptr)
      return VectorIndexNotConstant;
    if (static_cast<size_t>(C->getValue()) >= typeNumElements(VecType))
      return VectorIndexNotInRange;
    if (Index->getType() != Ice::IceType_i32)
      return VectorIndexNotI32;
    return VectorIndexValid;
  }

  // Returns true if the Str begins with Prefix.
  bool isStringPrefix(Ice::IceString &Str, Ice::IceString &Prefix) {
    const size_t PrefixSize = Prefix.size();
    if (Str.size() < PrefixSize)
      return false;
    for (size_t i = 0; i < PrefixSize; ++i) {
      if (Str[i] != Prefix[i])
        return false;
    }
    return true;
  }

  // Takes the PNaCl bitcode binary operator Opcode, and the opcode
  // type Ty, and sets Op to the corresponding ICE binary
  // opcode. Returns true if able to convert, false otherwise.
  bool convertBinopOpcode(unsigned Opcode, Ice::Type Ty,
                          Ice::InstArithmetic::OpKind &Op) {
    switch (Opcode) {
    default: {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Binary opcode " << Opcode << "not understood for type " << Ty;
      Error(StrBuf.str());
      // TODO(kschimpf) Remove error recovery once implementation complete.
      Op = Ice::InstArithmetic::Add;
      return false;
    }
    case naclbitc::BINOP_ADD:
      if (Ice::isIntegerType(Ty)) {
        Op = Ice::InstArithmetic::Add;
        return isValidIntegerArithOp(Op, Ty);
      } else {
        Op = Ice::InstArithmetic::Fadd;
        return isValidFloatingArithOp(Op, Ty);
      }
    case naclbitc::BINOP_SUB:
      if (Ice::isIntegerType(Ty)) {
        Op = Ice::InstArithmetic::Sub;
        return isValidIntegerArithOp(Op, Ty);
      } else {
        Op = Ice::InstArithmetic::Fsub;
        return isValidFloatingArithOp(Op, Ty);
      }
    case naclbitc::BINOP_MUL:
      if (Ice::isIntegerType(Ty)) {
        Op = Ice::InstArithmetic::Mul;
        return isValidIntegerArithOp(Op, Ty);
      } else {
        Op = Ice::InstArithmetic::Fmul;
        return isValidFloatingArithOp(Op, Ty);
      }
    case naclbitc::BINOP_UDIV:
      Op = Ice::InstArithmetic::Udiv;
      return isValidIntegerArithOp(Op, Ty);
    case naclbitc::BINOP_SDIV:
      if (Ice::isIntegerType(Ty)) {
        Op = Ice::InstArithmetic::Sdiv;
        return isValidIntegerArithOp(Op, Ty);
      } else {
        Op = Ice::InstArithmetic::Fdiv;
        return isValidFloatingArithOp(Op, Ty);
      }
    case naclbitc::BINOP_UREM:
      Op = Ice::InstArithmetic::Urem;
      return isValidIntegerArithOp(Op, Ty);
    case naclbitc::BINOP_SREM:
      if (Ice::isIntegerType(Ty)) {
        Op = Ice::InstArithmetic::Srem;
        return isValidIntegerArithOp(Op, Ty);
      } else {
        Op = Ice::InstArithmetic::Frem;
        return isValidFloatingArithOp(Op, Ty);
      }
    case naclbitc::BINOP_SHL:
      Op = Ice::InstArithmetic::Shl;
      return isValidIntegerArithOp(Op, Ty);
    case naclbitc::BINOP_LSHR:
      Op = Ice::InstArithmetic::Lshr;
      return isValidIntegerArithOp(Op, Ty);
    case naclbitc::BINOP_ASHR:
      Op = Ice::InstArithmetic::Ashr;
      return isValidIntegerArithOp(Op, Ty);
    case naclbitc::BINOP_AND:
      Op = Ice::InstArithmetic::And;
      return isValidIntegerLogicalOp(Op, Ty);
    case naclbitc::BINOP_OR:
      Op = Ice::InstArithmetic::Or;
      return isValidIntegerLogicalOp(Op, Ty);
    case naclbitc::BINOP_XOR:
      Op = Ice::InstArithmetic::Xor;
      return isValidIntegerLogicalOp(Op, Ty);
    }
  }

  /// Simplifies out vector types from Type1 and Type2, if both are vectors
  /// of the same size. Returns true iff both are vectors of the same size,
  /// or are both scalar types.
  static bool simplifyOutCommonVectorType(Ice::Type &Type1, Ice::Type &Type2) {
    bool IsType1Vector = isVectorType(Type1);
    bool IsType2Vector = isVectorType(Type2);
    if (IsType1Vector != IsType2Vector)
      return false;
    if (!IsType1Vector)
      return true;
    if (typeNumElements(Type1) != typeNumElements(Type2))
      return false;
    Type1 = typeElementType(Type1);
    Type2 = typeElementType(Type2);
    return true;
  }

  /// Returns true iff an integer truncation from SourceType to TargetType is
  /// valid.
  static bool isIntTruncCastValid(Ice::Type SourceType, Ice::Type TargetType) {
    return Ice::isIntegerType(SourceType)
        && Ice::isIntegerType(TargetType)
        && simplifyOutCommonVectorType(SourceType, TargetType)
        && getScalarIntBitWidth(SourceType) > getScalarIntBitWidth(TargetType);
  }

  /// Returns true iff a floating type truncation from SourceType to TargetType
  /// is valid.
  static bool isFloatTruncCastValid(Ice::Type SourceType,
                                    Ice::Type TargetType) {
    return simplifyOutCommonVectorType(SourceType, TargetType)
        && SourceType == Ice::IceType_f64
        && TargetType == Ice::IceType_f32;
  }

  /// Returns true iff an integer extension from SourceType to TargetType is
  /// valid.
  static bool isIntExtCastValid(Ice::Type SourceType, Ice::Type TargetType) {
    return isIntTruncCastValid(TargetType, SourceType);
  }

  /// Returns true iff a floating type extension from SourceType to TargetType
  /// is valid.
  static bool isFloatExtCastValid(Ice::Type SourceType, Ice::Type TargetType) {
    return isFloatTruncCastValid(TargetType, SourceType);
  }

  /// Returns true iff a cast from floating type SourceType to integer
  /// type TargetType is valid.
  static bool isFloatToIntCastValid(Ice::Type SourceType,
                                    Ice::Type TargetType) {
    if (!(Ice::isFloatingType(SourceType) && Ice::isIntegerType(TargetType)))
      return false;
    bool IsSourceVector = isVectorType(SourceType);
    bool IsTargetVector = isVectorType(TargetType);
    if (IsSourceVector != IsTargetVector)
      return false;
    if (IsSourceVector) {
      return typeNumElements(SourceType) == typeNumElements(TargetType);
    }
    return true;
  }

  /// Returns true iff a cast from integer type SourceType to floating
  /// type TargetType is valid.
  static bool isIntToFloatCastValid(Ice::Type SourceType,
                                    Ice::Type TargetType) {
    return isFloatToIntCastValid(TargetType, SourceType);
  }

  /// Returns the number of bits used to model type Ty when defining the
  /// bitcast instruction.
  static Ice::SizeT bitcastSizeInBits(Ice::Type Ty) {
    if (Ice::isVectorType(Ty))
      return Ice::typeNumElements(Ty) *
             bitcastSizeInBits(Ice::typeElementType(Ty));
    if (Ty == Ice::IceType_i1)
      return 1;
    return Ice::typeWidthInBytes(Ty) * CHAR_BIT;
  }

  /// Returns true iff a bitcast from SourceType to TargetType is allowed.
  static bool isBitcastValid(Ice::Type SourceType, Ice::Type TargetType) {
    return bitcastSizeInBits(SourceType) == bitcastSizeInBits(TargetType);
  }

  /// Returns true iff the NaCl bitcode Opcode is a valid cast opcode
  /// for converting SourceType to TargetType. Updates CastKind to the
  /// corresponding instruction cast opcode. Also generates an error
  /// message when this function returns false.
  bool convertCastOpToIceOp(uint64_t Opcode, Ice::Type SourceType,
                            Ice::Type TargetType,
                            Ice::InstCast::OpKind &CastKind) {
    bool Result;
    switch (Opcode) {
    default: {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Cast opcode " << Opcode << " not understood.\n";
      Error(StrBuf.str());
      // TODO(kschimpf) Remove error recovery once implementation complete.
      CastKind = Ice::InstCast::Bitcast;
      return false;
    }
    case naclbitc::CAST_TRUNC:
      CastKind = Ice::InstCast::Trunc;
      Result = isIntTruncCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_ZEXT:
      CastKind = Ice::InstCast::Zext;
      Result = isIntExtCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_SEXT:
      CastKind = Ice::InstCast::Sext;
      Result = isIntExtCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_FPTOUI:
      CastKind = Ice::InstCast::Fptoui;
      Result = isFloatToIntCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_FPTOSI:
      CastKind = Ice::InstCast::Fptosi;
      Result = isFloatToIntCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_UITOFP:
      CastKind = Ice::InstCast::Uitofp;
      Result = isIntToFloatCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_SITOFP:
      CastKind = Ice::InstCast::Sitofp;
      Result = isIntToFloatCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_FPTRUNC:
      CastKind = Ice::InstCast::Fptrunc;
      Result = isFloatTruncCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_FPEXT:
      CastKind = Ice::InstCast::Fpext;
      Result = isFloatExtCastValid(SourceType, TargetType);
      break;
    case naclbitc::CAST_BITCAST:
      CastKind = Ice::InstCast::Bitcast;
      Result = isBitcastValid(SourceType, TargetType);
      break;
    }
    if (!Result) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Illegal cast: " << Ice::InstCast::getCastName(CastKind) << " "
             << SourceType << " to " << TargetType;
      Error(StrBuf.str());
    }
    return Result;
  }

  // Converts PNaCl bitcode Icmp operator to corresponding ICE op.
  // Returns true if able to convert, false otherwise.
  bool convertNaClBitcICmpOpToIce(uint64_t Op,
                                  Ice::InstIcmp::ICond &Cond) const {
    switch (Op) {
    case naclbitc::ICMP_EQ:
      Cond = Ice::InstIcmp::Eq;
      return true;
    case naclbitc::ICMP_NE:
      Cond = Ice::InstIcmp::Ne;
      return true;
    case naclbitc::ICMP_UGT:
      Cond = Ice::InstIcmp::Ugt;
      return true;
    case naclbitc::ICMP_UGE:
      Cond = Ice::InstIcmp::Uge;
      return true;
    case naclbitc::ICMP_ULT:
      Cond = Ice::InstIcmp::Ult;
      return true;
    case naclbitc::ICMP_ULE:
      Cond = Ice::InstIcmp::Ule;
      return true;
    case naclbitc::ICMP_SGT:
      Cond = Ice::InstIcmp::Sgt;
      return true;
    case naclbitc::ICMP_SGE:
      Cond = Ice::InstIcmp::Sge;
      return true;
    case naclbitc::ICMP_SLT:
      Cond = Ice::InstIcmp::Slt;
      return true;
    case naclbitc::ICMP_SLE:
      Cond = Ice::InstIcmp::Sle;
      return true;
    default:
      // Make sure Cond is always initialized.
      Cond = static_cast<Ice::InstIcmp::ICond>(0);
      return false;
    }
  }

  // Converts PNaCl bitcode Fcmp operator to corresponding ICE op.
  // Returns true if able to convert, false otherwise.
  bool convertNaClBitcFCompOpToIce(uint64_t Op,
                                   Ice::InstFcmp::FCond &Cond) const {
    switch (Op) {
    case naclbitc::FCMP_FALSE:
      Cond = Ice::InstFcmp::False;
      return true;
    case naclbitc::FCMP_OEQ:
      Cond = Ice::InstFcmp::Oeq;
      return true;
    case naclbitc::FCMP_OGT:
      Cond = Ice::InstFcmp::Ogt;
      return true;
    case naclbitc::FCMP_OGE:
      Cond = Ice::InstFcmp::Oge;
      return true;
    case naclbitc::FCMP_OLT:
      Cond = Ice::InstFcmp::Olt;
      return true;
    case naclbitc::FCMP_OLE:
      Cond = Ice::InstFcmp::Ole;
      return true;
    case naclbitc::FCMP_ONE:
      Cond = Ice::InstFcmp::One;
      return true;
    case naclbitc::FCMP_ORD:
      Cond = Ice::InstFcmp::Ord;
      return true;
    case naclbitc::FCMP_UNO:
      Cond = Ice::InstFcmp::Uno;
      return true;
    case naclbitc::FCMP_UEQ:
      Cond = Ice::InstFcmp::Ueq;
      return true;
    case naclbitc::FCMP_UGT:
      Cond = Ice::InstFcmp::Ugt;
      return true;
    case naclbitc::FCMP_UGE:
      Cond = Ice::InstFcmp::Uge;
      return true;
    case naclbitc::FCMP_ULT:
      Cond = Ice::InstFcmp::Ult;
      return true;
    case naclbitc::FCMP_ULE:
      Cond = Ice::InstFcmp::Ule;
      return true;
    case naclbitc::FCMP_UNE:
      Cond = Ice::InstFcmp::Une;
      return true;
    case naclbitc::FCMP_TRUE:
      Cond = Ice::InstFcmp::True;
      return true;
    default:
      // Make sure Cond is always initialized.
      Cond = static_cast<Ice::InstFcmp::FCond>(0);
      return false;
    }
  }

  // Creates an error instruction, generating a value of type Ty, and
  // adds a placeholder so that instruction indices line up.
  // Some instructions, such as a call, will not generate a value
  // if the return type is void. In such cases, a placeholder value
  // for the badly formed instruction is not needed. Hence, if Ty is
  // void, an error instruction is not appended.
  // TODO(kschimpf) Remove error recovery once implementation complete.
  void appendErrorInstruction(Ice::Type Ty) {
    // Note: we don't worry about downstream translation errors because
    // the function will not be translated if any errors occur.
    if (Ty == Ice::IceType_void)
      return;
    Ice::Variable *Var = getNextInstVar(Ty);
    CurrentNode->appendInst(Ice::InstAssign::create(Func, Var, Var));
  }
};

void FunctionParser::ExitBlock() {
  if (isIRGenerationDisabled()) {
    popTimerIfTimingEachFunction();
    return;
  }
  // Before translating, check for blocks without instructions, and
  // insert unreachable. This shouldn't happen, but be safe.
  unsigned Index = 0;
  for (Ice::CfgNode *Node : Func->getNodes()) {
    if (Node->getInsts().empty()) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Basic block " << Index << " contains no instructions";
      Error(StrBuf.str());
      // TODO(kschimpf) Remove error recovery once implementation complete.
      Node->appendInst(Ice::InstUnreachable::create(Func));
    }
    ++Index;
  }
  Func->computePredecessors();
  // Note: Once any errors have been found, we turn off all
  // translation of all remaining functions. This allows use to see
  // multiple errors, without adding extra checks to the translator
  // for such parsing errors.
  if (Context->getNumErrors() == 0)
    getTranslator().translateFcn(Func);
  popTimerIfTimingEachFunction();
}

void FunctionParser::ReportInvalidBinaryOp(Ice::InstArithmetic::OpKind Op,
                                           Ice::Type OpTy) {
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "Invalid operator type for " << Ice::InstArithmetic::getOpName(Op)
         << ". Found " << OpTy;
  Error(StrBuf.str());
}

void FunctionParser::ProcessRecord() {
  // Note: To better separate parse/IR generation times, when IR generation
  // is disabled we do the following:
  // 1) Delay exiting until after we extract operands.
  // 2) return before we access operands, since all operands will be a nullptr.
  const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
  if (InstIsTerminating) {
    InstIsTerminating = false;
    if (!isIRGenerationDisabled())
      CurrentNode = getBasicBlock(++CurrentBbIndex);
  }
  // The base index for relative indexing.
  int32_t BaseIndex = getNextInstIndex();
  switch (Record.GetCode()) {
  case naclbitc::FUNC_CODE_DECLAREBLOCKS: {
    // DECLAREBLOCKS: [n]
    if (!isValidRecordSize(1, "count"))
      return;
    uint32_t NumBbs = Values[0];
    if (NumBbs == 0) {
      Error("Functions must contain at least one basic block.");
      // TODO(kschimpf) Remove error recovery once implementation complete.
      NumBbs = 1;
    }
    if (isIRGenerationDisabled())
      return;
    if (Func->getNodes().size() != 1) {
      Error("Duplicate function block count record");
      return;
    }
    // Install the basic blocks, skipping bb0 which was created in the
    // constructor.
    for (size_t i = 1; i < NumBbs; ++i)
      InstallNextBasicBlock();
    return;
  }
  case naclbitc::FUNC_CODE_INST_BINOP: {
    // BINOP: [opval, opval, opcode]
    if (!isValidRecordSize(3, "binop"))
      return;
    Ice::Operand *Op1 = getRelativeOperand(Values[0], BaseIndex);
    Ice::Operand *Op2 = getRelativeOperand(Values[1], BaseIndex);
    if (isIRGenerationDisabled()) {
      assert(Op1 == nullptr && Op2 == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    Ice::Type Type1 = Op1->getType();
    Ice::Type Type2 = Op2->getType();
    if (Type1 != Type2) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Binop argument types differ: " << Type1 << " and " << Type2;
      Error(StrBuf.str());
      appendErrorInstruction(Type1);
      return;
    }

    Ice::InstArithmetic::OpKind Opcode;
    if (!convertBinopOpcode(Values[2], Type1, Opcode)) {
      appendErrorInstruction(Type1);
      return;
    }
    CurrentNode->appendInst(Ice::InstArithmetic::create(
        Func, Opcode, getNextInstVar(Type1), Op1, Op2));
    return;
  }
  case naclbitc::FUNC_CODE_INST_CAST: {
    // CAST: [opval, destty, castopc]
    if (!isValidRecordSize(3, "cast"))
      return;
    Ice::Operand *Src = getRelativeOperand(Values[0], BaseIndex);
    Ice::Type CastType = Context->getSimpleTypeByID(Values[1]);
    Ice::InstCast::OpKind CastKind;
    if (isIRGenerationDisabled()) {
      assert(Src == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    if (!convertCastOpToIceOp(Values[2], Src->getType(), CastType, CastKind)) {
      appendErrorInstruction(CastType);
      return;
    }
    CurrentNode->appendInst(
        Ice::InstCast::create(Func, CastKind, getNextInstVar(CastType), Src));
    return;
  }
  case naclbitc::FUNC_CODE_INST_VSELECT: {
    // VSELECT: [opval, opval, pred]
    if (!isValidRecordSize(3, "select"))
      return;
    Ice::Operand *ThenVal = getRelativeOperand(Values[0], BaseIndex);
    Ice::Operand *ElseVal = getRelativeOperand(Values[1], BaseIndex);
    Ice::Operand *CondVal = getRelativeOperand(Values[2], BaseIndex);
    if (isIRGenerationDisabled()) {
      assert(ThenVal == nullptr && ElseVal == nullptr && CondVal == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    Ice::Type ThenType = ThenVal->getType();
    Ice::Type ElseType = ElseVal->getType();
    if (ThenType != ElseType) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Select operands not same type. Found " << ThenType << " and "
             << ElseType;
      Error(StrBuf.str());
      appendErrorInstruction(ThenType);
      return;
    }
    Ice::Type CondType = CondVal->getType();
    if (isVectorType(CondType)) {
      if (!isVectorType(ThenType) ||
          typeElementType(CondType) != Ice::IceType_i1 ||
          typeNumElements(ThenType) != typeNumElements(CondType)) {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Select condition type " << CondType
               << " not allowed for values of type " << ThenType;
        Error(StrBuf.str());
        appendErrorInstruction(ThenType);
        return;
      }
    } else if (CondVal->getType() != Ice::IceType_i1) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Select condition " << CondVal << " not type i1. Found: "
             << CondVal->getType();
      Error(StrBuf.str());
      appendErrorInstruction(ThenType);
      return;
    }
    CurrentNode->appendInst(Ice::InstSelect::create(
        Func, getNextInstVar(ThenType), CondVal, ThenVal, ElseVal));
    return;
  }
  case naclbitc::FUNC_CODE_INST_EXTRACTELT: {
    // EXTRACTELT: [opval, opval]
    if (!isValidRecordSize(2, "extract element"))
      return;
    Ice::Operand *Vec = getRelativeOperand(Values[0], BaseIndex);
    Ice::Operand *Index = getRelativeOperand(Values[1], BaseIndex);
    if (isIRGenerationDisabled()) {
      assert(Vec == nullptr && Index == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    Ice::Type VecType = Vec->getType();
    VectorIndexCheckValue IndexCheckValue = validateVectorIndex(Vec, Index);
    if (IndexCheckValue != VectorIndexValid) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      dumpVectorIndexCheckValue(StrBuf, IndexCheckValue);
      StrBuf << ": extractelement " << VecType << " " << *Vec << ", "
             << Index->getType() << " " << *Index;
      Error(StrBuf.str());
      appendErrorInstruction(VecType);
      return;
    }
    CurrentNode->appendInst(Ice::InstExtractElement::create(
        Func, getNextInstVar(typeElementType(VecType)), Vec, Index));
    return;
  }
  case naclbitc::FUNC_CODE_INST_INSERTELT: {
    // INSERTELT: [opval, opval, opval]
    if (!isValidRecordSize(3, "insert element"))
      return;
    Ice::Operand *Vec = getRelativeOperand(Values[0], BaseIndex);
    Ice::Operand *Elt = getRelativeOperand(Values[1], BaseIndex);
    Ice::Operand *Index = getRelativeOperand(Values[2], BaseIndex);
    if (isIRGenerationDisabled()) {
      assert(Vec == nullptr && Elt == nullptr && Index == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    Ice::Type VecType = Vec->getType();
    VectorIndexCheckValue IndexCheckValue = validateVectorIndex(Vec, Index);
    if (IndexCheckValue != VectorIndexValid) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      dumpVectorIndexCheckValue(StrBuf, IndexCheckValue);
      StrBuf << ": insertelement " << VecType << " " << *Vec << ", "
             << Elt->getType() << " " << *Elt << ", " << Index->getType() << " "
             << *Index;
      Error(StrBuf.str());
      appendErrorInstruction(Elt->getType());
      return;
    }
    CurrentNode->appendInst(Ice::InstInsertElement::create(
        Func, getNextInstVar(VecType), Vec, Elt, Index));
    return;
  }
  case naclbitc::FUNC_CODE_INST_CMP2: {
    // CMP2: [opval, opval, pred]
    if (!isValidRecordSize(3, "compare"))
      return;
    Ice::Operand *Op1 = getRelativeOperand(Values[0], BaseIndex);
    Ice::Operand *Op2 = getRelativeOperand(Values[1], BaseIndex);
    if (isIRGenerationDisabled()) {
      assert(Op1 == nullptr && Op2 == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    Ice::Type Op1Type = Op1->getType();
    Ice::Type Op2Type = Op2->getType();
    Ice::Type DestType = getCompareResultType(Op1Type);
    if (Op1Type != Op2Type) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Compare argument types differ: " << Op1Type
             << " and " << Op2Type;
      Error(StrBuf.str());
      appendErrorInstruction(DestType);
      Op2 = Op1;
    }
    if (DestType == Ice::IceType_void) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Compare not defined for type " << Op1Type;
      Error(StrBuf.str());
      return;
    }
    Ice::Variable *Dest = getNextInstVar(DestType);
    if (isIntegerType(Op1Type)) {
      Ice::InstIcmp::ICond Cond;
      if (!convertNaClBitcICmpOpToIce(Values[2], Cond)) {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Compare record contains unknown integer predicate index: "
               << Values[2];
        Error(StrBuf.str());
        appendErrorInstruction(DestType);
      }
      CurrentNode->appendInst(
          Ice::InstIcmp::create(Func, Cond, Dest, Op1, Op2));
    } else if (isFloatingType(Op1Type)){
      Ice::InstFcmp::FCond Cond;
      if (!convertNaClBitcFCompOpToIce(Values[2], Cond)) {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Compare record contains unknown float predicate index: "
               << Values[2];
        Error(StrBuf.str());
        appendErrorInstruction(DestType);
      }
      CurrentNode->appendInst(
          Ice::InstFcmp::create(Func, Cond, Dest, Op1, Op2));
    } else {
      // Not sure this can happen, but be safe.
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Compare on type not understood: " << Op1Type;
      Error(StrBuf.str());
      appendErrorInstruction(DestType);
      return;
    }
    return;
  }
  case naclbitc::FUNC_CODE_INST_RET: {
    // RET: [opval?]
    if (!isValidRecordSizeInRange(0, 1, "return"))
      return;
    if (Values.empty()) {
      if (isIRGenerationDisabled())
        return;
      CurrentNode->appendInst(Ice::InstRet::create(Func));
    } else {
      Ice::Operand *RetVal = getRelativeOperand(Values[0], BaseIndex);
      if (isIRGenerationDisabled()) {
        assert(RetVal == nullptr);
        return;
      }
      CurrentNode->appendInst(Ice::InstRet::create(Func, RetVal));
    }
    InstIsTerminating = true;
    return;
  }
  case naclbitc::FUNC_CODE_INST_BR: {
    if (Values.size() == 1) {
      // BR: [bb#]
      if (isIRGenerationDisabled())
        return;
      Ice::CfgNode *Block = getBranchBasicBlock(Values[0]);
      if (Block == nullptr)
        return;
      CurrentNode->appendInst(Ice::InstBr::create(Func, Block));
    } else {
      // BR: [bb#, bb#, opval]
      if (!isValidRecordSize(3, "branch"))
        return;
      Ice::Operand *Cond = getRelativeOperand(Values[2], BaseIndex);
      if (isIRGenerationDisabled()) {
        assert(Cond == nullptr);
        return;
      }
      if (Cond->getType() != Ice::IceType_i1) {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Branch condition " << *Cond << " not i1. Found: "
               << Cond->getType();
        Error(StrBuf.str());
        return;
      }
      Ice::CfgNode *ThenBlock = getBranchBasicBlock(Values[0]);
      Ice::CfgNode *ElseBlock = getBranchBasicBlock(Values[1]);
      if (ThenBlock == nullptr || ElseBlock == nullptr)
        return;
      CurrentNode->appendInst(
          Ice::InstBr::create(Func, Cond, ThenBlock, ElseBlock));
    }
    InstIsTerminating = true;
    return;
  }
  case naclbitc::FUNC_CODE_INST_SWITCH: {
    // SWITCH: [Condty, Cond, BbIndex, NumCases Case ...]
    // where Case = [1, 1, Value, BbIndex].
    //
    // Note: Unlike most instructions, we don't infer the type of
    // Cond, but provide it as a separate field. There are also
    // unnecesary data fields (i.e. constants 1).  These were not
    // cleaned up in PNaCl bitcode because the bitcode format was
    // already frozen when the problem was noticed.
    if (!isValidRecordSizeAtLeast(4, "switch"))
      return;

    Ice::Type CondTy = Context->getSimpleTypeByID(Values[0]);
    if (!Ice::isScalarIntegerType(CondTy)) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Case condition must be non-wide integer. Found: " << CondTy;
      Error(StrBuf.str());
      return;
    }
    Ice::SizeT BitWidth = Ice::getScalarIntBitWidth(CondTy);
    Ice::Operand *Cond = getRelativeOperand(Values[1], BaseIndex);

    const bool isIRGenDisabled = isIRGenerationDisabled();
    if (isIRGenDisabled) {
      assert(Cond == nullptr);
    } else if (CondTy != Cond->getType()) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Case condition expects type " << CondTy
             << ". Found: " << Cond->getType();
      Error(StrBuf.str());
      return;
    }
    Ice::CfgNode *DefaultLabel =
        isIRGenDisabled ? nullptr : getBranchBasicBlock(Values[2]);
    unsigned NumCases = Values[3];

    // Now recognize each of the cases.
    if (!isValidRecordSize(4 + NumCases * 4, "switch"))
      return;
    Ice::InstSwitch *Switch =
        isIRGenDisabled ? nullptr : Ice::InstSwitch::create(Func, NumCases,
                                                            Cond, DefaultLabel);
    unsigned ValCaseIndex = 4;  // index to beginning of case entry.
    for (unsigned CaseIndex = 0; CaseIndex < NumCases;
         ++CaseIndex, ValCaseIndex += 4) {
      if (Values[ValCaseIndex] != 1 || Values[ValCaseIndex+1] != 1) {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Sequence [1, 1, value, label] expected for case entry "
               << "in switch record. (at index" << ValCaseIndex << ")";
        Error(StrBuf.str());
        return;
      }
      Ice::APInt Value(BitWidth,
                       NaClDecodeSignRotatedValue(Values[ValCaseIndex + 2]));
      if (isIRGenDisabled)
        continue;
      Ice::CfgNode *Label = getBranchBasicBlock(Values[ValCaseIndex + 3]);
      Switch->addBranch(CaseIndex, Value.getSExtValue(), Label);
    }
    if (isIRGenDisabled)
      return;
    CurrentNode->appendInst(Switch);
    InstIsTerminating = true;
    return;
  }
  case naclbitc::FUNC_CODE_INST_UNREACHABLE: {
    // UNREACHABLE: []
    if (!isValidRecordSize(0, "unreachable"))
      return;
    if (isIRGenerationDisabled())
      return;
    CurrentNode->appendInst(
        Ice::InstUnreachable::create(Func));
    InstIsTerminating = true;
    return;
  }
  case naclbitc::FUNC_CODE_INST_PHI: {
    // PHI: [ty, val1, bb1, ..., valN, bbN] for n >= 2.
    if (!isValidRecordSizeAtLeast(3, "phi"))
      return;
    Ice::Type Ty = Context->getSimpleTypeByID(Values[0]);
    if ((Values.size() & 0x1) == 0) {
      // Not an odd number of values.
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "function block phi record size not valid: " << Values.size();
      Error(StrBuf.str());
      appendErrorInstruction(Ty);
      return;
    }
    if (Ty == Ice::IceType_void) {
      Error("Phi record using type void not allowed");
      return;
    }
    if (isIRGenerationDisabled()) {
      // Verify arguments are defined before quitting.
      for (unsigned i = 1; i < Values.size(); i += 2) {
        assert(getRelativeOperand(NaClDecodeSignRotatedValue(Values[i]),
                                  BaseIndex) == nullptr);
      }
      setNextLocalInstIndex(nullptr);
      return;
    }
    Ice::Variable *Dest = getNextInstVar(Ty);
    Ice::InstPhi *Phi = Ice::InstPhi::create(Func, Values.size() >> 1, Dest);
    for (unsigned i = 1; i < Values.size(); i += 2) {
      Ice::Operand *Op =
          getRelativeOperand(NaClDecodeSignRotatedValue(Values[i]), BaseIndex);
      if (Op->getType() != Ty) {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Value " << *Op << " not type " << Ty
               << " in phi instruction. Found: " << Op->getType();
        Error(StrBuf.str());
        appendErrorInstruction(Ty);
        return;
      }
      Phi->addArgument(Op, getBasicBlock(Values[i + 1]));
    }
    CurrentNode->appendInst(Phi);
    return;
  }
  case naclbitc::FUNC_CODE_INST_ALLOCA: {
    // ALLOCA: [Size, align]
    if (!isValidRecordSize(2, "alloca"))
      return;
    Ice::Operand *ByteCount = getRelativeOperand(Values[0], BaseIndex);
    uint32_t Alignment;
    extractAlignment("Alloca", Values[1], Alignment);
    if (isIRGenerationDisabled()) {
      assert(ByteCount == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    Ice::Type PtrTy = Ice::getPointerType();
    if (ByteCount->getType() != Ice::IceType_i32) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Alloca on non-i32 value. Found: " << *ByteCount;
      Error(StrBuf.str());
      appendErrorInstruction(PtrTy);
      return;
    }
    CurrentNode->appendInst(Ice::InstAlloca::create(Func, ByteCount, Alignment,
                                                    getNextInstVar(PtrTy)));
    return;
  }
  case naclbitc::FUNC_CODE_INST_LOAD: {
    // LOAD: [address, align, ty]
    if (!isValidRecordSize(3, "load"))
      return;
    Ice::Operand *Address = getRelativeOperand(Values[0], BaseIndex);
    Ice::Type Ty = Context->getSimpleTypeByID(Values[2]);
    uint32_t Alignment;
    extractAlignment("Load", Values[1], Alignment);
    if (isIRGenerationDisabled()) {
      assert(Address == nullptr);
      setNextLocalInstIndex(nullptr);
      return;
    }
    if (!isValidPointerType(Address, "Load")) {
      appendErrorInstruction(Ty);
      return;
    }
    if (!isValidLoadStoreAlignment(Alignment, Ty, "Load")) {
      appendErrorInstruction(Ty);
      return;
    }
    CurrentNode->appendInst(
        Ice::InstLoad::create(Func, getNextInstVar(Ty), Address, Alignment));
    return;
  }
  case naclbitc::FUNC_CODE_INST_STORE: {
    // STORE: [address, value, align]
    if (!isValidRecordSize(3, "store"))
      return;
    Ice::Operand *Address = getRelativeOperand(Values[0], BaseIndex);
    Ice::Operand *Value = getRelativeOperand(Values[1], BaseIndex);
    uint32_t Alignment;
    extractAlignment("Store", Values[2], Alignment);
    if (isIRGenerationDisabled()) {
      assert(Address == nullptr && Value == nullptr);
      return;
    }
    if (!isValidPointerType(Address, "Store"))
      return;
    if (!isValidLoadStoreAlignment(Alignment, Value->getType(), "Store"))
      return;
    CurrentNode->appendInst(
        Ice::InstStore::create(Func, Value, Address, Alignment));
    return;
  }
  case naclbitc::FUNC_CODE_INST_CALL:
  case naclbitc::FUNC_CODE_INST_CALL_INDIRECT: {
    // CALL: [cc, fnid, arg0, arg1...]
    // CALL_INDIRECT: [cc, fn, returnty, args...]
    //
    // Note: The difference between CALL and CALL_INDIRECT is that
    // CALL has a reference to an explicit function declaration, while
    // the CALL_INDIRECT is just an address. For CALL, we can infer
    // the return type by looking up the type signature associated
    // with the function declaration. For CALL_INDIRECT we can only
    // infer the type signature via argument types, and the
    // corresponding return type stored in CALL_INDIRECT record.
    Ice::SizeT ParamsStartIndex = 2;
    if (Record.GetCode() == naclbitc::FUNC_CODE_INST_CALL) {
      if (!isValidRecordSizeAtLeast(2, "call"))
        return;
    } else {
      if (!isValidRecordSizeAtLeast(3, "call indirect"))
        return;
      ParamsStartIndex = 3;
    }

    // Extract out the called function and its return type.
    uint32_t CalleeIndex = convertRelativeToAbsIndex(Values[1], BaseIndex);
    Ice::Operand *Callee = getOperand(CalleeIndex);
    Ice::Type ReturnType = Ice::IceType_void;
    const Ice::Intrinsics::FullIntrinsicInfo *IntrinsicInfo = nullptr;
    if (Record.GetCode() == naclbitc::FUNC_CODE_INST_CALL) {
      Ice::FunctionDeclaration *Fcn = Context->getFunctionByID(CalleeIndex);
      const Ice::FuncSigType &Signature = Fcn->getSignature();
      ReturnType = Signature.getReturnType();

      // Check if this direct call is to an Intrinsic (starts with "llvm.")
      static Ice::IceString LLVMPrefix("llvm.");
      Ice::IceString Name = Fcn->getName();
      if (isStringPrefix(Name, LLVMPrefix)) {
        Ice::IceString Suffix = Name.substr(LLVMPrefix.size());
        IntrinsicInfo =
            getTranslator().getContext()->getIntrinsicsInfo().find(Suffix);
        if (!IntrinsicInfo) {
          std::string Buffer;
          raw_string_ostream StrBuf(Buffer);
          StrBuf << "Invalid PNaCl intrinsic call to " << Name;
          Error(StrBuf.str());
          appendErrorInstruction(ReturnType);
          return;
        }
      }
    } else {
      ReturnType = Context->getSimpleTypeByID(Values[2]);
    }

    // Extract call information.
    uint64_t CCInfo = Values[0];
    CallingConv::ID CallingConv;
    if (!naclbitc::DecodeCallingConv(CCInfo >> 1, CallingConv)) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Function call calling convention value " << (CCInfo >> 1)
             << " not understood.";
      Error(StrBuf.str());
      appendErrorInstruction(ReturnType);
      return;
    }
    bool IsTailCall = static_cast<bool>(CCInfo & 1);
    Ice::SizeT NumParams = Values.size() - ParamsStartIndex;

    if (isIRGenerationDisabled()) {
      assert(Callee == nullptr);
      // Check that parameters are defined.
      for (Ice::SizeT ParamIndex = 0; ParamIndex < NumParams; ++ParamIndex) {
        assert(getRelativeOperand(Values[ParamsStartIndex + ParamIndex],
                                  BaseIndex) == nullptr);
      }
      // Define value slot only if value returned.
      if (ReturnType != Ice::IceType_void)
        setNextLocalInstIndex(nullptr);
      return;
    }

    // Create the call instruction.
    Ice::Variable *Dest = (ReturnType == Ice::IceType_void)
                              ? nullptr
                              : getNextInstVar(ReturnType);
    Ice::InstCall *Inst = nullptr;
    if (IntrinsicInfo) {
      Inst =
          Ice::InstIntrinsicCall::create(Func, NumParams, Dest, Callee,
                                         IntrinsicInfo->Info);
    } else {
      Inst = Ice::InstCall::create(Func, NumParams, Dest, Callee, IsTailCall);
    }

    // Add parameters.
    for (Ice::SizeT ParamIndex = 0; ParamIndex < NumParams; ++ParamIndex) {
      Inst->addArg(
          getRelativeOperand(Values[ParamsStartIndex + ParamIndex], BaseIndex));
    }

    // If intrinsic call, validate call signature.
    if (IntrinsicInfo) {
      Ice::SizeT ArgIndex = 0;
      switch (IntrinsicInfo->validateCall(Inst, ArgIndex)) {
      default:
        Error("Unknown validation error for intrinsic call");
        // TODO(kschimpf) Remove error recovery once implementation complete.
        break;
      case Ice::Intrinsics::IsValidCall:
        break;
      case Ice::Intrinsics::BadReturnType: {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Intrinsic call expects return type "
               << IntrinsicInfo->getReturnType()
               << ". Found: " << Inst->getReturnType();
        Error(StrBuf.str());
        // TODO(kschimpf) Remove error recovery once implementation complete.
        break;
      }
      case Ice::Intrinsics::WrongNumOfArgs: {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Intrinsic call expects " << IntrinsicInfo->getNumArgs()
               << ". Found: " << Inst->getNumArgs();
        Error(StrBuf.str());
        // TODO(kschimpf) Remove error recovery once implementation complete.
        break;
      }
      case Ice::Intrinsics::WrongCallArgType: {
        std::string Buffer;
        raw_string_ostream StrBuf(Buffer);
        StrBuf << "Intrinsic call argument " << ArgIndex << " expects type "
               << IntrinsicInfo->getArgType(ArgIndex)
               << ". Found: " << Inst->getArg(ArgIndex)->getType();
        Error(StrBuf.str());
        // TODO(kschimpf) Remove error recovery once implementation complete.
        break;
      }
      }
    }

    CurrentNode->appendInst(Inst);
    return;
  }
  case naclbitc::FUNC_CODE_INST_FORWARDTYPEREF: {
    // FORWARDTYPEREF: [opval, ty]
    if (!isValidRecordSize(2, "forward type ref"))
      return;
    Ice::Type OpType = Context->getSimpleTypeByID(Values[1]);
    setOperand(Values[0],
               isIRGenerationDisabled() ? nullptr : createInstVar(OpType));
    return;
  }
  default:
    // Generate error message!
    BlockParserBaseClass::ProcessRecord();
    return;
  }
}

/// Parses constants within a function block.
class ConstantsParser : public BlockParserBaseClass {
  ConstantsParser(const ConstantsParser &) = delete;
  ConstantsParser &operator=(const ConstantsParser &) = delete;

public:
  ConstantsParser(unsigned BlockID, FunctionParser *FuncParser)
      : BlockParserBaseClass(BlockID, FuncParser),
        Timer(Ice::TimerStack::TT_parseConstants, getTranslator().getContext()),
        FuncParser(FuncParser), NextConstantType(Ice::IceType_void) {}

  ~ConstantsParser() override {}

  const char *getBlockName() const override { return "constants"; }

private:
  Ice::TimerMarker Timer;
  // The parser of the function block this constants block appears in.
  FunctionParser *FuncParser;
  // The type to use for succeeding constants.
  Ice::Type NextConstantType;

  void ProcessRecord() override;

  Ice::GlobalContext *getContext() { return getTranslator().getContext(); }

  // Returns true if the type to use for succeeding constants is defined.
  // If false, also generates an error message.
  bool isValidNextConstantType() {
    if (NextConstantType != Ice::IceType_void)
      return true;
    Error("Constant record not preceded by set type record");
    return false;
  }
};

void ConstantsParser::ProcessRecord() {
  const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
  switch (Record.GetCode()) {
  case naclbitc::CST_CODE_SETTYPE: {
    // SETTYPE: [typeid]
    if (!isValidRecordSize(1, "set type"))
      return;
    NextConstantType = Context->getSimpleTypeByID(Values[0]);
    if (NextConstantType == Ice::IceType_void)
      Error("constants block set type not allowed for void type");
    return;
  }
  case naclbitc::CST_CODE_UNDEF: {
    // UNDEF
    if (!isValidRecordSize(0, "undef"))
      return;
    if (!isValidNextConstantType())
      return;
    if (isIRGenerationDisabled()) {
      FuncParser->setNextConstantID(nullptr);
      return;
    }
    FuncParser->setNextConstantID(
        getContext()->getConstantUndef(NextConstantType));
    return;
  }
  case naclbitc::CST_CODE_INTEGER: {
    // INTEGER: [intval]
    if (!isValidRecordSize(1, "integer"))
      return;
    if (!isValidNextConstantType())
      return;
    if (isIRGenerationDisabled()) {
      FuncParser->setNextConstantID(nullptr);
      return;
    }
    if (Ice::isScalarIntegerType(NextConstantType)) {
      Ice::APInt Value(Ice::getScalarIntBitWidth(NextConstantType),
                       NaClDecodeSignRotatedValue(Values[0]));
      if (Ice::Constant *C = getContext()->getConstantInt(
              NextConstantType, Value.getSExtValue())) {
        FuncParser->setNextConstantID(C);
        return;
      }
    }
    std::string Buffer;
    raw_string_ostream StrBuf(Buffer);
    StrBuf << "constant block integer record for non-integer type "
           << NextConstantType;
    Error(StrBuf.str());
    return;
  }
  case naclbitc::CST_CODE_FLOAT: {
    // FLOAT: [fpval]
    if (!isValidRecordSize(1, "float"))
      return;
    if (!isValidNextConstantType())
      return;
    if (isIRGenerationDisabled()) {
      FuncParser->setNextConstantID(nullptr);
      return;
    }
    switch (NextConstantType) {
    case Ice::IceType_f32: {
      const Ice::APInt IntValue(32, static_cast<uint32_t>(Values[0]));
      float FpValue = Ice::convertAPIntToFp<int32_t, float>(IntValue);
      FuncParser->setNextConstantID(getContext()->getConstantFloat(FpValue));
      return;
    }
    case Ice::IceType_f64: {
      const Ice::APInt IntValue(64, Values[0]);
      double FpValue = Ice::convertAPIntToFp<uint64_t, double>(IntValue);
      FuncParser->setNextConstantID(getContext()->getConstantDouble(FpValue));
      return;
    }
    default: {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "constant block float record for non-floating type "
             << NextConstantType;
      Error(StrBuf.str());
      return;
    }
    }
  }
  default:
    // Generate error message!
    BlockParserBaseClass::ProcessRecord();
    return;
  }
}

// Parses valuesymtab blocks appearing in a function block.
class FunctionValuesymtabParser : public ValuesymtabParser {
  FunctionValuesymtabParser(const FunctionValuesymtabParser &) = delete;
  void operator=(const FunctionValuesymtabParser &) = delete;

public:
  FunctionValuesymtabParser(unsigned BlockID, FunctionParser *EnclosingParser)
      : ValuesymtabParser(BlockID, EnclosingParser),
        Timer(Ice::TimerStack::TT_parseFunctionValuesymtabs,
              getTranslator().getContext()) {}

private:
  Ice::TimerMarker Timer;
  // Returns the enclosing function parser.
  FunctionParser *getFunctionParser() const {
    return reinterpret_cast<FunctionParser *>(GetEnclosingParser());
  }

  void setValueName(uint64_t Index, StringType &Name) override;
  void setBbName(uint64_t Index, StringType &Name) override;

  // Reports that the assignment of Name to the value associated with
  // index is not possible, for the given Context.
  void reportUnableToAssign(const char *Context, uint64_t Index,
                            StringType &Name) {
    std::string Buffer;
    raw_string_ostream StrBuf(Buffer);
    StrBuf << "Function-local " << Context << " name '" << Name
           << "' can't be associated with index " << Index;
    Error(StrBuf.str());
  }
};

void FunctionValuesymtabParser::setValueName(uint64_t Index, StringType &Name) {
  // Note: We check when Index is too small, so that we can error recover
  // (FP->getOperand will create fatal error).
  if (Index < getFunctionParser()->getNumGlobalIDs()) {
    reportUnableToAssign("instruction", Index, Name);
    // TODO(kschimpf) Remove error recovery once implementation complete.
    return;
  }
  if (isIRGenerationDisabled())
    return;
  Ice::Operand *Op = getFunctionParser()->getOperand(Index);
  if (Ice::Variable *V = dyn_cast<Ice::Variable>(Op)) {
    if (ALLOW_DUMP) {
      std::string Nm(Name.data(), Name.size());
      V->setName(getFunctionParser()->getFunc(), Nm);
    }
  } else {
    reportUnableToAssign("variable", Index, Name);
  }
}

void FunctionValuesymtabParser::setBbName(uint64_t Index, StringType &Name) {
  if (isIRGenerationDisabled())
    return;
  if (Index >= getFunctionParser()->getFunc()->getNumNodes()) {
    reportUnableToAssign("block", Index, Name);
    return;
  }
  std::string Nm(Name.data(), Name.size());
  if (ALLOW_DUMP)
    getFunctionParser()->getFunc()->getNodes()[Index]->setName(Nm);
}

bool FunctionParser::ParseBlock(unsigned BlockID) {
  switch (BlockID) {
  case naclbitc::CONSTANTS_BLOCK_ID: {
    ConstantsParser Parser(BlockID, this);
    return Parser.ParseThisBlock();
  }
  case naclbitc::VALUE_SYMTAB_BLOCK_ID: {
    if (PNaClAllowLocalSymbolTables) {
      FunctionValuesymtabParser Parser(BlockID, this);
      return Parser.ParseThisBlock();
    }
    break;
  }
  default:
    break;
  }
  return BlockParserBaseClass::ParseBlock(BlockID);
}

/// Parses the module block in the bitcode file.
class ModuleParser : public BlockParserBaseClass {
public:
  ModuleParser(unsigned BlockID, TopLevelParser *Context)
      : BlockParserBaseClass(BlockID, Context),
        Timer(Ice::TimerStack::TT_parseModule,
              Context->getTranslator().getContext()),
        GlobalDeclarationNamesAndInitializersInstalled(false) {}

  ~ModuleParser() override {}

  const char *getBlockName() const override { return "module"; }

private:
  Ice::TimerMarker Timer;
  // True if we have already installed names for unnamed global declarations,
  // and have generated global constant initializers.
  bool GlobalDeclarationNamesAndInitializersInstalled;

  // Generates names for unnamed global addresses (i.e. functions and
  // global variables). Then lowers global variable declaration
  // initializers to the target. May be called multiple times. Only
  // the first call will do the installation.
  void InstallGlobalNamesAndGlobalVarInitializers() {
    if (!GlobalDeclarationNamesAndInitializersInstalled) {
      Ice::Translator &Trans = getTranslator();
      const Ice::IceString &GlobalPrefix = getFlags().DefaultGlobalPrefix;
      if (!GlobalPrefix.empty()) {
        uint32_t NameIndex = 0;
        for (Ice::VariableDeclaration *Var : Context->getGlobalVariables()) {
          installDeclarationName(Trans, Var, GlobalPrefix, "global", NameIndex);
        }
      }
      const Ice::IceString &FunctionPrefix = getFlags().DefaultFunctionPrefix;
      if (!FunctionPrefix.empty()) {
        uint32_t NameIndex = 0;
        for (Ice::FunctionDeclaration *Func :
             Context->getFunctionDeclarationList()) {
          installDeclarationName(Trans, Func, FunctionPrefix, "function",
                                 NameIndex);
        }
      }
      getTranslator().lowerGlobals(Context->getGlobalVariables());
      GlobalDeclarationNamesAndInitializersInstalled = true;
    }
  }

  void installDeclarationName(Ice::Translator &Trans,
                              Ice::GlobalDeclaration *Decl,
                              const Ice::IceString &Prefix, const char *Context,
                              uint32_t &NameIndex) {
    if (!Decl->hasName()) {
      Decl->setName(Trans.createUnnamedName(Prefix, NameIndex));
      ++NameIndex;
    } else {
      Trans.checkIfUnnamedNameSafe(Decl->getName(), Context, Prefix,
                                   Trans.getContext()->getStrDump());
    }
  }

  bool ParseBlock(unsigned BlockID) override;

  void ExitBlock() override {
    InstallGlobalNamesAndGlobalVarInitializers();
    getTranslator().emitConstants();
  }

  void ProcessRecord() override;
};

class ModuleValuesymtabParser : public ValuesymtabParser {
  ModuleValuesymtabParser(const ModuleValuesymtabParser &) = delete;
  void operator=(const ModuleValuesymtabParser &) = delete;

public:
  ModuleValuesymtabParser(unsigned BlockID, ModuleParser *MP)
      : ValuesymtabParser(BlockID, MP),
        Timer(Ice::TimerStack::TT_parseModuleValuesymtabs,
              getTranslator().getContext()) {}

  ~ModuleValuesymtabParser() override {}

private:
  Ice::TimerMarker Timer;
  void setValueName(uint64_t Index, StringType &Name) override;
  void setBbName(uint64_t Index, StringType &Name) override;
};

void ModuleValuesymtabParser::setValueName(uint64_t Index, StringType &Name) {
  Context->getGlobalDeclarationByID(Index)
      ->setName(StringRef(Name.data(), Name.size()));
}

void ModuleValuesymtabParser::setBbName(uint64_t Index, StringType &Name) {
  std::string Buffer;
  raw_string_ostream StrBuf(Buffer);
  StrBuf << "Can't define basic block name at global level: '" << Name
         << "' -> " << Index;
  Error(StrBuf.str());
}

bool ModuleParser::ParseBlock(unsigned BlockID) {
  switch (BlockID) {
  case naclbitc::BLOCKINFO_BLOCK_ID:
    return NaClBitcodeParser::ParseBlock(BlockID);
  case naclbitc::TYPE_BLOCK_ID_NEW: {
    TypesParser Parser(BlockID, this);
    return Parser.ParseThisBlock();
  }
  case naclbitc::GLOBALVAR_BLOCK_ID: {
    GlobalsParser Parser(BlockID, this);
    return Parser.ParseThisBlock();
  }
  case naclbitc::VALUE_SYMTAB_BLOCK_ID: {
    ModuleValuesymtabParser Parser(BlockID, this);
    return Parser.ParseThisBlock();
  }
  case naclbitc::FUNCTION_BLOCK_ID: {
    InstallGlobalNamesAndGlobalVarInitializers();
    FunctionParser Parser(BlockID, this);
    return Parser.ParseThisBlock();
  }
  default:
    return BlockParserBaseClass::ParseBlock(BlockID);
  }
}

void ModuleParser::ProcessRecord() {
  const NaClBitcodeRecord::RecordVector &Values = Record.GetValues();
  switch (Record.GetCode()) {
  case naclbitc::MODULE_CODE_VERSION: {
    // VERSION: [version#]
    if (!isValidRecordSize(1, "version"))
      return;
    unsigned Version = Values[0];
    if (Version != 1) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Unknown bitstream version: " << Version;
      Error(StrBuf.str());
    }
    return;
  }
  case naclbitc::MODULE_CODE_FUNCTION: {
    // FUNCTION:  [type, callingconv, isproto, linkage]
    if (!isValidRecordSize(4, "address"))
      return;
    const Ice::FuncSigType &Signature = Context->getFuncSigTypeByID(Values[0]);
    CallingConv::ID CallingConv;
    if (!naclbitc::DecodeCallingConv(Values[1], CallingConv)) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Function address has unknown calling convention: "
             << Values[1];
      Error(StrBuf.str());
      return;
    }
    GlobalValue::LinkageTypes Linkage;
    if (!naclbitc::DecodeLinkage(Values[3], Linkage)) {
      std::string Buffer;
      raw_string_ostream StrBuf(Buffer);
      StrBuf << "Function address has unknown linkage. Found " << Values[3];
      Error(StrBuf.str());
      return;
    }
    Ice::FunctionDeclaration *Func = Ice::FunctionDeclaration::create(
        getTranslator().getContext(), Signature, CallingConv, Linkage,
        Values[2] == 0);
    if (Values[2] == 0)
      Context->setNextValueIDAsImplementedFunction();
    Context->setNextFunctionID(Func);
    return;
  }
  default:
    BlockParserBaseClass::ProcessRecord();
    return;
  }
}

bool TopLevelParser::ParseBlock(unsigned BlockID) {
  if (BlockID == naclbitc::MODULE_BLOCK_ID) {
    ModuleParser Parser(BlockID, this);
    return Parser.ParseThisBlock();
  }
  // Generate error message by using default block implementation.
  BlockParserBaseClass Parser(BlockID, this);
  return Parser.ParseThisBlock();
}

} // end of anonymous namespace

namespace Ice {

void PNaClTranslator::translate(const std::string &IRFilename) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> ErrOrFile =
      MemoryBuffer::getFileOrSTDIN(IRFilename);
  if (std::error_code EC = ErrOrFile.getError()) {
    errs() << "Error reading '" << IRFilename << "': " << EC.message() << "\n";
    ErrorStatus = true;
    return;
  }

  std::unique_ptr<MemoryBuffer> MemBuf(ErrOrFile.get().release());
  if (MemBuf->getBufferSize() % 4 != 0) {
    errs() << IRFilename
           << ": Bitcode stream should be a multiple of 4 bytes in length.\n";
    ErrorStatus = true;
    return;
  }

  const unsigned char *BufPtr = (const unsigned char *)MemBuf->getBufferStart();
  const unsigned char *EndBufPtr = BufPtr + MemBuf->getBufferSize();

  // Read header and verify it is good.
  NaClBitcodeHeader Header;
  if (Header.Read(BufPtr, EndBufPtr) || !Header.IsSupported()) {
    errs() << "Invalid PNaCl bitcode header.\n";
    ErrorStatus = true;
    return;
  }

  // Create a bitstream reader to read the bitcode file.
  NaClBitstreamReader InputStreamFile(BufPtr, EndBufPtr);
  NaClBitstreamCursor InputStream(InputStreamFile);

  TopLevelParser Parser(*this, Header, InputStream, ErrorStatus);
  int TopLevelBlocks = 0;
  while (!InputStream.AtEndOfStream()) {
    if (Parser.Parse()) {
      ErrorStatus = true;
      return;
    }
    ++TopLevelBlocks;
  }

  if (TopLevelBlocks != 1) {
    errs() << IRFilename
           << ": Contains more than one module. Found: " << TopLevelBlocks
           << "\n";
    ErrorStatus = true;
  }
}

} // end of namespace Ice
