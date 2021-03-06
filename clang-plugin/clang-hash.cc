#include "hash-visitor.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <fstream>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include "Hash.h"

using namespace clang;
using namespace llvm;

static std::chrono::high_resolution_clock::time_point StartCompilation;

static enum {
  ATEXIT_NOP,
  ATEXIT_FROM_CACHE,
  ATEXIT_TO_CACHE,
} atexit_mode = ATEXIT_NOP;

static char *hashfile = NULL;
static char *hash_new = NULL;

static char const *objectfile = NULL;
static char *objectfile_copy = NULL;

static void link_object_file() {
  if (atexit_mode == ATEXIT_NOP) {
    return;
  }
  assert(objectfile != nullptr);
  assert(objectfile_copy != nullptr);

  char const *src = nullptr, *dst = nullptr;

  if (atexit_mode == ATEXIT_FROM_CACHE) {
    src = objectfile_copy;
    dst = objectfile;
  } else {
    src = objectfile;
    dst = objectfile_copy;
  }

  // Record Events
  if (getenv("CLANG_HASH_LOGFILE")) {
    int fd =
        open(getenv("CLANG_HASH_LOGFILE"), O_APPEND | O_WRONLY | O_CREAT, 0644);
    const char *fail = (atexit_mode == ATEXIT_FROM_CACHE ? "H" : "M");
    write(fd, fail, 1);
    close(fd);
  }

  /* If destination exists, we have to unlink it. */
  struct stat dummy;
  if (stat(dst, &dummy) == 0) { // exists
    if (unlink(dst) != 0) {     // unlink failed
      perror("clang-hash: unlink objectfile/objectfile copy");
      return;
    }
  }
  if (stat(src, &dummy) != 0) { // src exists
    perror("clang-hash: source objectfile/objectfile copy does not exist");
    return;
  }

  // Copy by hardlink
  if (link(src, dst) != 0) {
    perror("clang-hash: objectfile update failed");
    return;
  }

  // Write Hash to hashfile
  if (atexit_mode == ATEXIT_TO_CACHE) {
    if (hashfile != NULL) {
      FILE *f = fopen(hashfile, "w+");
      fwrite(hash_new, strlen(hash_new), 1, f);
      fclose(f);
    }
  } else if (atexit_mode == ATEXIT_FROM_CACHE) {
    // Update Timestamp
    utime(dst, NULL);
  }
}

struct ObjectCache {
  std::string m_cachedir;
  raw_ostream *m_terminal;

  ObjectCache(std::string cachedir, raw_ostream *Terminal)
      : m_cachedir(cachedir), m_terminal(Terminal) {}

  char *hash_filename(std::string objectfile) {
    if (m_cachedir == "") {
      std::string path(objectfile + ".hash");
      return strdup(path.c_str());
    }
    return NULL;
  }

  char *objectcopy_filename(std::string objectfile, std::string hash) {
    if (m_cachedir == "") {
      std::string path(objectfile + ".hash.copy");
      return strdup(path.c_str());
    }
    std::string dir(m_cachedir + "/" + hash.substr(0, 2));
    mkdir(dir.c_str(), 0755);
    std::string path(dir + "/" + hash.substr(2) + ".o");
    return strdup(path.c_str());
  }

  std::string find_object_from_hash(std::string objectfile, std::string hash) {
    if (m_cachedir != "") {
      std::string ObjectPath(m_cachedir + "/" + hash.substr(0, 2) + "/" +
                             hash.substr(2) + ".o");
      struct stat dummy;
      if (stat(ObjectPath.c_str(), &dummy) == 0) {
        // Found!
        return ObjectPath;
      }
      return "";
    } else {
      std::string OldHash;
      std::string HashPath(objectfile + ".hash");
      std::string ObjectPath(objectfile + ".hash.copy");
      std::ifstream FileStream(HashPath);
      if (FileStream.good()) {
        getline(FileStream, OldHash);
        if (m_terminal) {
          (*m_terminal) << HashPath << ": old hash string: " << OldHash << "\n";
        }
      } else {
        if (m_terminal) {
          (*m_terminal) << "Warning: could not open file \"" << HashPath
                        << "\", cannot read previous hash.\n";
        }
        return "";
      }

      // Hashes are equal, try to find the objectfile
      if (hash == OldHash) {
        struct stat dummy;
        if (stat(ObjectPath.c_str(), &dummy) == 0) {
          // Found!
          return ObjectPath;
        }
      }
      return "";
    }
  }
};

class DefinitionUseVisitor
    : public RecursiveASTVisitor<DefinitionUseVisitor> {
    typedef RecursiveASTVisitor<DefinitionUseVisitor> Inherited;
public:

    std::map<const Decl *, std::set<const Decl *>> DefUseSilo;
    const Decl* CurrentDefinition;

    bool TraverseDecl(Decl *D) {
        if (!D) return true;
        bool record = false;
        if (isa<VarDecl>(D) && static_cast<VarDecl*>(D)->hasGlobalStorage()) {
            CurrentDefinition = D;
            record = true;
        }
        if (isa<FunctionDecl>(D)) {
            CurrentDefinition = D;
            record = true;
        }

        bool ret = Inherited::TraverseDecl(D);
        if (record) {
            CurrentDefinition = nullptr;
        }
        return ret;
    }

    bool VisitDeclRefExpr(const DeclRefExpr *Node) {
        const ValueDecl * ValDecl = Node->getDecl();
        if (!CurrentDefinition) return true;
        if (!ValDecl) return true;

        if (isa<VarDecl>(ValDecl)) {
            const VarDecl *VD = static_cast<const VarDecl *>(ValDecl);
            if (VD->hasGlobalStorage()) {
                DefUseSilo[CurrentDefinition].insert(VD);
            }
        } else if (isa<FunctionDecl>(ValDecl)) {
            DefUseSilo[CurrentDefinition].insert(ValDecl);
        }
        return true;
    }

    bool VisitCallExpr(CallExpr *Node) {
        if (!CurrentDefinition) return true;
        if (FunctionDecl * FD = Node->getDirectCallee()) {
            DefUseSilo[CurrentDefinition].insert(FD);
        }
        return true;
    }

};


class HashTranslationUnitConsumer : public ASTConsumer {
public:
  HashTranslationUnitConsumer(CompilerInstance &CI, raw_ostream *OS,
                              bool StopIfSameHash)
      : CI(CI), Terminal(OS), StopIfSameHash(StopIfSameHash) {}

    typedef llvm::MurMur3 Hash;
    typedef llvm::MurMur3::Digest HashResult;

  virtual void HandleTranslationUnit(clang::ASTContext &Context) override {
    /// Step 1: Calculate Hash
    const auto StartHashing = std::chrono::high_resolution_clock::now();

    TranslationUnitDecl *TU = Context.getTranslationUnitDecl();

    // Traversing the translation unit decl via a RecursiveASTVisitor
    // will visit all nodes in the AST.
    CHashVisitor<Hash, HashResult> Visitor(Context);
    Visitor.TraverseDecl(TU);

    unsigned ProcessedBytes;
    /* The Translation Unit hash contains not only the AST hash, but
     * also the command line arguments */
    Hash TUHash;
    HashResult TUHashResult;
    const HashResult *ASTHash = Visitor.getHash(TU);
    TUHash.update(ASTHash->Bytes);
    hashCommandLineArguments(TUHash);


    TUHash.final(TUHashResult);
    std::string HashString = TUHashResult.digest().str();
    hash_new = strdup(HashString.c_str());

    const auto FinishHashing = std::chrono::high_resolution_clock::now();

    /* Record all uses of interessting definitions for clang-global-hash */
    DefinitionUseVisitor DefUse;
    DefUse.TraverseDecl(TU);

    char *cachedir = getenv("CLANG_HASH_CACHE");
    ObjectCache cache(cachedir ? cachedir : "", Terminal);

    // Step 2: Consequent Handling
    bool HashEqual;
    if (Context.getSourceManager().getDiagnostics().hasErrorOccurred()) {
      HashEqual = false;
    } else {
      if (objectfile == nullptr) {
        objectfile = "";
      }
      assert(objectfile != nullptr); // the next line (string ctor) throws
                                     // exception if objectfile is nullptr
      std::string copy = cache.find_object_from_hash(objectfile, HashString);
      if (copy != "") {
        HashEqual = true;
        objectfile_copy = strdup(copy.c_str());
      } else {
        HashEqual = false;
      }
    }

    // Step 2.1: -hash-verbose
    // Sometimes we do terminal output
    if (Terminal) {
      *Terminal << "hash-start-time-ns "
                << std::chrono::duration_cast<std::chrono::nanoseconds>(
                       StartHashing.time_since_epoch()).count() << "\n";
      *Terminal << "top-level-hash: " << HashString << "\n";
      *Terminal << "processed-bytes: " << ProcessedBytes << "\n";
      *Terminal << "parse-time-ns: "
                << std::chrono::duration_cast<std::chrono::nanoseconds>(
                       StartHashing - StartCompilation).count() << "\n";
      *Terminal << "hash-time-ns: "
                << std::chrono::duration_cast<std::chrono::nanoseconds>(
                       FinishHashing - StartHashing).count() << "\n";
      *Terminal << "element-hashes: [";
      for (const auto &SavedHash : Visitor.DeclSilo) {
        const Decl *D = SavedHash.first;
        const HashResult &Dig = SavedHash.second;
        // Only Top-level declarations
        if (D->getDeclContext() &&
            isa<TranslationUnitDecl>(D->getDeclContext()) &&
            isa<NamedDecl>(D)) {
          const bool IsFunctionDefinition =
              isa<FunctionDecl>(D) &&
              cast<FunctionDecl>(D)->isThisDeclarationADefinition();
          const bool IsNonExternVariableDeclaration =
              isa<VarDecl>(D) && !cast<VarDecl>(D)->hasExternalStorage();

          bool AppendFilename = false;
          if (IsFunctionDefinition) { // Ignore declarations without definition
            if (cast<FunctionDecl>(D)->getStorageClass() == SC_Static) {
              *Terminal << "(\"static function:";
              AppendFilename = true;
            } else {
              *Terminal << "(\"function:";
            }
          } else if (IsNonExternVariableDeclaration) { // Ignore extern
                                                       // variables
            if (cast<VarDecl>(D)->getStorageClass() == SC_Static) {
              *Terminal << "(\"static variable:";
              AppendFilename = true;
            } else {
              *Terminal << "(\"variable:";
            }
          } else if (isa<RecordDecl>(D)) {
            *Terminal << "(\"record:";
            AppendFilename = true;
          } else
            continue;

          if (cast<NamedDecl>(D)->getName() != "") {
            *Terminal << cast<NamedDecl>(D)->getName();
          } else if (auto TD = cast<TypeDecl>(D)) {
            // If the name is empty, use the typedef'ed name (or the generic
            // identifier provided by the compiler).
            // This happens e.g. when a struct is unnamed (and may or may not be
            // typedef'ed at definition).
            *Terminal << TD->getTypeForDecl()
                             ->getCanonicalTypeInternal()
                             .getAsString();
          }
          if (AppendFilename) {
            // Append the filename to the symbol's name
            const auto Filename =
                CI.getSourceManager().getFilename(D->getLocation());
            *Terminal << ":" << (Filename.startswith("./")
                                     ? Filename.slice(2, Filename.size())
                                     : Filename);
          }
          *Terminal << "\", \"";
          *Terminal << Dig.digest().str();
          *Terminal << "\"";

          if (IsFunctionDefinition || IsNonExternVariableDeclaration) {
            *Terminal << ", [";
            for (const auto &SavedCallee : DefUse.DefUseSilo[cast<Decl>(D)]) {
              // TODO: also dump records? could be forward-declarated?!
              bool AppendFilename = false;
              if (isa<FunctionDecl>(SavedCallee)) {
                if (cast<FunctionDecl>(SavedCallee)->getStorageClass() ==
                    SC_Static) {
                  *Terminal << "\"static function:";
                  AppendFilename = true;
                } else
                  *Terminal << "\"function:";
              } else {
                if (cast<VarDecl>(SavedCallee)->getStorageClass() ==
                    SC_Static) {
                  *Terminal << "\"static variable:";
                  AppendFilename = true;
                } else
                  *Terminal << "\"variable:";
              }
              *Terminal << cast<NamedDecl>(SavedCallee)->getName();
              if (AppendFilename) {
                // Append the filename to the symbol's name
                const auto Filename = CI.getSourceManager().getFilename(
                    SavedCallee->getLocation());
                *Terminal << ":" << (Filename.startswith("./")
                                         ? Filename.slice(2, Filename.size())
                                         : Filename);
              }
              *Terminal << "\", ";
            }
            *Terminal << "]";
          }

          *Terminal << "), ";
        }
      }
      *Terminal << "]\n";
      *Terminal << "hash-equal:" << HashEqual << "\n";
      *Terminal << "skipped:" << (HashEqual && StopIfSameHash) << "\n";
    }

    if (StopIfSameHash && objectfile != nullptr) {
      // We are in caching mode and there should be an objectfile
      atexit(link_object_file);
      if (HashEqual) {
        CI.clearOutputFiles(true);
        atexit_mode = ATEXIT_FROM_CACHE;
        exit(0);
      } else {
        hashfile = cache.hash_filename(objectfile);
        objectfile_copy = cache.objectcopy_filename(objectfile, HashString);
        atexit_mode = ATEXIT_TO_CACHE;
        // Continue with compilation
      }
    }
  }

private:
  // Returns true if the -stop-if-same-hash flag is set, else false.
  void hashCommandLineArguments(Hash &TUHash) {
    // Get command line arguments
    const std::string PPID{std::to_string(getppid())};
    const std::string FilePath = "/proc/" + PPID + "/cmdline";
    std::ifstream CommandLine{FilePath};
    if (CommandLine.good()) { // TODO: move this to own method?
      std::list<std::string> CommandLineArgs;
      std::string Arg;
      do {
        getline(CommandLine, Arg, '\0');
        if ("-o" == Arg) {
          // throw away next parameter (name of outfile)
          getline(CommandLine, Arg, '\0');
          continue;
        }
        if (Arg.size() > 2 && Arg.compare(Arg.size() - 2, 2, ".c") == 0)
          continue;
        if (Arg.size() > 2 && Arg.compare(Arg.size() - 2, 2, ".i") == 0)
          continue;

        if (Arg.find("-stop-if-same-hash") != std::string::npos) {
          continue; // also don't hash this (plugin argument)
        }
        if (Arg == "-I") {
          // throw away next parameter (include path)
          getline(CommandLine, Arg, '\0');
          continue;
        }
        if (Arg.substr(0, 2) == "-I") {
          continue; // also don't hash include paths
        }
        if (Arg.substr(0, 2) == "-D") {
          continue; // also don't hash macro defines
        }
        if (Arg.find("-hash-verbose") != std::string::npos) {
          continue; // also don't hash this (plugin argument)
        }

        TUHash.update(Arg);
      } while (Arg.size());

    } else {
      errs() << "Warning: could not open file \"" << FilePath
             << "\", cannot hash command line arguments.\n";
    }
  }

  CompilerInstance &CI;
  raw_ostream *const Terminal;
  bool StopIfSameHash;
};

class HashTranslationUnitAction : public PluginASTAction {
protected:
  bool StopIfSameHash;
  bool Verbose;

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef) override {
    const std::string &OutputFile = CI.getFrontendOpts().OutputFile;
    if (OutputFile != "" && OutputFile != "/dev/null") {
      std::string tmp;
      objectfile = strdup(OutputFile.c_str());
    }

    // Write hash database to .o.hash if the compiler produces a object file
    if ((CI.getFrontendOpts().ProgramAction == frontend::EmitObj ||
         CI.getFrontendOpts().ProgramAction == frontend::EmitBC ||
         CI.getFrontendOpts().ProgramAction == frontend::EmitLLVM) &&
        OutputFile != "" && OutputFile != "/dev/null") {
      /* OK.Let's run */
    } else if (CI.getFrontendOpts().ProgramAction ==
               frontend::ParseSyntaxOnly) {
      /* OK Let's run */
    } else {
      return make_unique<ASTConsumer>();
    }

    raw_ostream *Terminal = nullptr;
    if (Verbose)
      Terminal = &errs();

    return make_unique<HashTranslationUnitConsumer>(CI, Terminal,
                                                    StopIfSameHash);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string> &Args) override {
    StartCompilation = std::chrono::high_resolution_clock::now();
    Verbose = false;
    StopIfSameHash = false;
    for (const std::string &Arg : Args) {
      if (Arg == "-hash-verbose") {
        Verbose = true;
      }
      if (Arg == "-stop-if-same-hash") {
        StopIfSameHash = true;
      }
    }
    if (Args.size() && Args[0] == "help") {
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
    X("clang-hash", "hash translation unit");
