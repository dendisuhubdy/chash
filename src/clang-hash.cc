#include "hash-visitor.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace llvm;

class HashTranslationUnitConsumer : public ASTConsumer {
public:
  HashTranslationUnitConsumer(raw_ostream *OS) : TopLevelHashStream(OS) {}

  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
    // Traversing the translation unit decl via a RecursiveASTVisitor
    // will visit all nodes in the AST.
    Visitor.hashDecl(Context.getTranslationUnitDecl());

    // Context.getTranslationUnitDecl()->dump();
    unsigned ProcessedBytes;
    std::string HashString = Visitor.getHash(&ProcessedBytes);

    if (TopLevelHashStream) {
      TopLevelHashStream->write(HashString.c_str(), HashString.length());
      delete TopLevelHashStream;
    }
    errs() << "top-level-hash: " << HashString << "\n";
    errs() << "processed bytes: " << ProcessedBytes << "\n";
  }

private:
  raw_ostream *TopLevelHashStream;
  TranslationUnitHashVisitor Visitor;
};

class HashTranslationUnitAction : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef) override {
    // Write hash database to .o.hash if the compiler produces a object file
    raw_ostream *Out = nullptr;
    if (CI.getFrontendOpts().OutputFile != "") {
      std::error_code Error;
      std::string HashFile = CI.getFrontendOpts().OutputFile + ".hash";
      Out = new raw_fd_ostream(HashFile, Error, sys::fs::F_Text);
      errs() << "dump-ast-file: " << CI.getFrontendOpts().OutputFile << " "
             << HashFile << "\n";
      if (Error) {
        errs() << "Could not open ast-hash file: "
               << CI.getFrontendOpts().OutputFile << "\n";
      }
    }
    return make_unique<HashTranslationUnitConsumer>(Out);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string> &arg) override {
    for (const std::string &Arg : arg) {
      errs() << " arg = " << Arg << "\n";

      // Example error handling.
      if (Arg == "-an-error") {
        DiagnosticsEngine &D = CI.getDiagnostics();
        unsigned DiagID = D.getCustomDiagID(DiagnosticsEngine::Error,
                                            "invalid argument '%0'");
        D.Report(DiagID) << Arg;
        return false;
      }
    }
    if (arg.size() && arg[0] == "help") {
      // FIXME
      PrintHelp(errs());
    }
    return true;
  }

  PluginASTAction::ActionType getActionType() override {
    return AddBeforeMainAction;
  }

  void PrintHelp(raw_ostream &Out) {
    Out << "Help for PrintFunctionNames plugin goes here\n";
  }
};

static FrontendPluginRegistry::Add<HashTranslationUnitAction>
    X("hash-unit", "hash translation unit");
