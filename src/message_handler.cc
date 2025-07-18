// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "message_handler.hh"

#include "log.hh"
#include "pipeline.hh"
#include "project.hh"
#include "query.hh"

#include <rapidjson/document.h>
#include <rapidjson/reader.h>

#include <llvm/ADT/STLExtras.h>

#include <algorithm>
#include <stdexcept>

using namespace clang;

#if LLVM_VERSION_MAJOR < 15 // llvmorg-15-init-6118-gb39f43775796
namespace llvm {
template <typename T, typename E> constexpr bool is_contained(std::initializer_list<T> set, const E &e) {
  for (const T &v : set)
    if (v == e)
      return true;
  return false;
}
} // namespace llvm
#endif

MAKE_HASHABLE(ccls::SymbolIdx, t.usr, t.kind);

namespace ccls {
REFLECT_STRUCT(CodeActionParam::Context, diagnostics);
REFLECT_STRUCT(CodeActionParam, textDocument, range, context);
void reflect(JsonReader &, EmptyParam &) {}
REFLECT_STRUCT(TextDocumentParam, textDocument);
REFLECT_STRUCT(DidOpenTextDocumentParam, textDocument);
REFLECT_STRUCT(TextDocumentContentChangeEvent, range, rangeLength, text);
REFLECT_STRUCT(TextDocumentDidChangeParam, textDocument, contentChanges);
REFLECT_STRUCT(TextDocumentPositionParam, textDocument, position);
REFLECT_STRUCT(RenameParam, textDocument, position, newName);
REFLECT_STRUCT(CallsParam, item);

// completion
REFLECT_UNDERLYING(CompletionTriggerKind);
REFLECT_STRUCT(CompletionContext, triggerKind, triggerCharacter);
REFLECT_STRUCT(CompletionParam, textDocument, position, context);

// formatting
REFLECT_STRUCT(FormattingOptions, tabSize, insertSpaces);
REFLECT_STRUCT(DocumentFormattingParam, textDocument, options);
REFLECT_STRUCT(DocumentOnTypeFormattingParam, textDocument, position, ch, options);
REFLECT_STRUCT(DocumentRangeFormattingParam, textDocument, range, options);

// workspace
REFLECT_UNDERLYING(FileChangeType);
REFLECT_STRUCT(DidChangeWatchedFilesParam::Event, uri, type);
REFLECT_STRUCT(DidChangeWatchedFilesParam, changes);
REFLECT_STRUCT(DidChangeWorkspaceFoldersParam::Event, added, removed);
REFLECT_STRUCT(DidChangeWorkspaceFoldersParam, event);
REFLECT_STRUCT(WorkspaceSymbolParam, query, folders);

namespace {
struct Occur {
  lsRange range;
  Role role;
};
struct CclsSemanticHighlightSymbol {
  int id = 0;
  SymbolKind parentKind;
  SymbolKind kind;
  uint8_t storage;
  std::vector<std::pair<int, int>> ranges;

  // `lsOccur` is used to compute `ranges`.
  std::vector<Occur> lsOccurs;
};

struct CclsSemanticHighlight {
  DocumentUri uri;
  std::vector<CclsSemanticHighlightSymbol> symbols;
};
REFLECT_STRUCT(CclsSemanticHighlightSymbol, id, parentKind, kind, storage, ranges);
REFLECT_STRUCT(CclsSemanticHighlight, uri, symbols);

struct CclsSetSkippedRanges {
  DocumentUri uri;
  std::vector<lsRange> skippedRanges;
};
REFLECT_STRUCT(CclsSetSkippedRanges, uri, skippedRanges);

struct SemanticTokensPartialResult {
  std::vector<int> data;
};
REFLECT_STRUCT(SemanticTokensPartialResult, data);

struct ScanLineEvent {
  Position pos;
  Position end_pos; // Second key when there is a tie for insertion events.
  int id;
  Role role;
  CclsSemanticHighlightSymbol *symbol;
  bool operator<(const ScanLineEvent &o) const {
    // See the comments below when insertion/deletion events are inserted.
    if (!(pos == o.pos))
      return pos < o.pos;
    if (!(o.end_pos == end_pos))
      return o.end_pos < end_pos;
    // This comparison essentially order Macro after non-Macro,
    // So that macros will not be rendered as Var/Type/...
    if (symbol->kind != o.symbol->kind)
      return symbol->kind < o.symbol->kind;
    // If symbol A and B occupy the same place, we want one to be placed
    // before the other consistantly.
    return symbol->id < o.symbol->id;
  }
};
} // namespace

void ReplyOnce::notOpened(std::string_view path) {
  error(ErrorCode::InvalidRequest, std::string(path) + " is not opened");
}

void ReplyOnce::replyLocationLink(std::vector<LocationLink> &result) {
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  if (result.size() > g_config->xref.maxNum)
    result.resize(g_config->xref.maxNum);
  if (g_config->client.linkSupport) {
    (*this)(result);
  } else {
    (*this)(std::vector<Location>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end())));
  }
}

void MessageHandler::bind(const char *method, void (MessageHandler::*handler)(JsonReader &)) {
  method2notification[method] = [this, handler](JsonReader &reader) { (this->*handler)(reader); };
}

template <typename Param> void MessageHandler::bind(const char *method, void (MessageHandler::*handler)(Param &)) {
  method2notification[method] = [this, handler](JsonReader &reader) {
    Param param{};
    reflect(reader, param);
    (this->*handler)(param);
  };
}

void MessageHandler::bind(const char *method, void (MessageHandler::*handler)(JsonReader &, ReplyOnce &)) {
  method2request[method] = [this, handler](JsonReader &reader, ReplyOnce &reply) { (this->*handler)(reader, reply); };
}

template <typename Param>
void MessageHandler::bind(const char *method, void (MessageHandler::*handler)(Param &, ReplyOnce &)) {
  method2request[method] = [this, handler](JsonReader &reader, ReplyOnce &reply) {
    Param param{};
    reflect(reader, param);
    (this->*handler)(param, reply);
  };
}

MessageHandler::MessageHandler() {
  // clang-format off
  bind("$ccls/call", &MessageHandler::ccls_call);
  bind("$ccls/fileInfo", &MessageHandler::ccls_fileInfo);
  bind("$ccls/info", &MessageHandler::ccls_info);
  bind("$ccls/inheritance", &MessageHandler::ccls_inheritance);
  bind("$ccls/member", &MessageHandler::ccls_member);
  bind("$ccls/navigate", &MessageHandler::ccls_navigate);
  bind("$ccls/reload", &MessageHandler::ccls_reload);
  bind("$ccls/vars", &MessageHandler::ccls_vars);
  bind("callHierarchy/incomingCalls", &MessageHandler::callHierarchy_incomingCalls);
  bind("callHierarchy/outgoingCalls", &MessageHandler::callHierarchy_outgoingCalls);
  bind("exit", &MessageHandler::exit);
  bind("initialize", &MessageHandler::initialize);
  bind("initialized", &MessageHandler::initialized);
  bind("shutdown", &MessageHandler::shutdown);
  bind("textDocument/codeAction", &MessageHandler::textDocument_codeAction);
  bind("textDocument/codeLens", &MessageHandler::textDocument_codeLens);
  bind("textDocument/completion", &MessageHandler::textDocument_completion);
  bind("textDocument/declaration", &MessageHandler::textDocument_declaration);
  bind("textDocument/definition", &MessageHandler::textDocument_definition);
  bind("textDocument/didChange", &MessageHandler::textDocument_didChange);
  bind("textDocument/didClose", &MessageHandler::textDocument_didClose);
  bind("textDocument/didOpen", &MessageHandler::textDocument_didOpen);
  bind("textDocument/didSave", &MessageHandler::textDocument_didSave);
  bind("textDocument/documentHighlight", &MessageHandler::textDocument_documentHighlight);
  bind("textDocument/documentLink", &MessageHandler::textDocument_documentLink);
  bind("textDocument/documentSymbol", &MessageHandler::textDocument_documentSymbol);
  bind("textDocument/foldingRange", &MessageHandler::textDocument_foldingRange);
  bind("textDocument/formatting", &MessageHandler::textDocument_formatting);
  bind("textDocument/hover", &MessageHandler::textDocument_hover);
  bind("textDocument/implementation", &MessageHandler::textDocument_implementation);
  bind("textDocument/onTypeFormatting", &MessageHandler::textDocument_onTypeFormatting);
  bind("textDocument/prepareCallHierarchy", &MessageHandler::textDocument_prepareCallHierarchy);
  bind("textDocument/rangeFormatting", &MessageHandler::textDocument_rangeFormatting);
  bind("textDocument/references", &MessageHandler::textDocument_references);
  bind("textDocument/rename", &MessageHandler::textDocument_rename);
  bind("textDocument/semanticTokens/full", &MessageHandler::textDocument_semanticTokensFull);
  bind("textDocument/semanticTokens/range", &MessageHandler::textDocument_semanticTokensRange);
  bind("textDocument/signatureHelp", &MessageHandler::textDocument_signatureHelp);
  bind("textDocument/switchSourceHeader", &MessageHandler::textDocument_switchSourceHeader);
  bind("textDocument/typeDefinition", &MessageHandler::textDocument_typeDefinition);
  bind("workspace/didChangeConfiguration", &MessageHandler::workspace_didChangeConfiguration);
  bind("workspace/didChangeWatchedFiles", &MessageHandler::workspace_didChangeWatchedFiles);
  bind("workspace/didChangeWorkspaceFolders", &MessageHandler::workspace_didChangeWorkspaceFolders);
  bind("workspace/executeCommand", &MessageHandler::workspace_executeCommand);
  bind("workspace/symbol", &MessageHandler::workspace_symbol);
  // clang-format on
}

void MessageHandler::run(InMessage &msg) {
  rapidjson::Document &doc = *msg.document;
  rapidjson::Value null;
  auto it = doc.FindMember("params");
  JsonReader reader(it != doc.MemberEnd() ? &it->value : &null);
  if (msg.id.valid()) {
    ReplyOnce reply{*this, msg.id};
    auto it = method2request.find(msg.method);
    if (it != method2request.end()) {
      try {
        it->second(reader, reply);
      } catch (std::invalid_argument &ex) {
        reply.error(ErrorCode::InvalidParams,
                    "invalid params of " + msg.method + ": expected " + ex.what() + " for " + reader.getPath());
      } catch (NotIndexed &) {
        throw;
      } catch (...) {
        reply.error(ErrorCode::InternalError, "failed to process " + msg.method);
      }
    } else {
      reply.error(ErrorCode::MethodNotFound, "unknown request " + msg.method);
    }
  } else {
    auto it = method2notification.find(msg.method);
    if (it != method2notification.end())
      try {
        it->second(reader);
      } catch (...) {
        ShowMessageParam param{MessageType::Error, std::string("failed to process ") + msg.method};
        pipeline::notify(window_showMessage, param);
      }
  }
}

QueryFile *MessageHandler::findFile(const std::string &path, int *out_file_id) {
  QueryFile *ret = nullptr;
  auto it = db->name2file_id.find(lowerPathIfInsensitive(path));
  if (it != db->name2file_id.end()) {
    QueryFile &file = db->files[it->second];
    if (file.def) {
      ret = &file;
      if (out_file_id)
        *out_file_id = it->second;
      return ret;
    }
  }
  if (out_file_id)
    *out_file_id = -1;
  return ret;
}

std::pair<QueryFile *, WorkingFile *> MessageHandler::findOrFail(const std::string &path, ReplyOnce &reply,
                                                                 int *out_file_id, bool allow_unopened) {
  WorkingFile *wf = wfiles->getFile(path);
  if (!wf && !allow_unopened) {
    reply.notOpened(path);
    return {nullptr, nullptr};
  }
  QueryFile *file = findFile(path, out_file_id);
  if (!file) {
    if (!overdue)
      throw NotIndexed{path};
    reply.error(ErrorCode::InvalidRequest, "not indexed");
    return {nullptr, nullptr};
  }
  return {file, wf};
}

void emitSkippedRanges(WorkingFile *wfile, QueryFile &file) {
  CclsSetSkippedRanges params;
  params.uri = DocumentUri::fromPath(wfile->filename);
  for (Range skipped : file.def->skipped_ranges)
    if (auto ls_skipped = getLsRange(wfile, skipped))
      params.skippedRanges.push_back(*ls_skipped);
  pipeline::notify("$ccls/publishSkippedRanges", params);
}

static std::unordered_map<SymbolIdx, CclsSemanticHighlightSymbol> computeSemanticTokens(DB *db, WorkingFile *wfile,
                                                                                        QueryFile &file) {
  static GroupMatch match(g_config->highlight.whitelist, g_config->highlight.blacklist);
  assert(file.def);
  // Group symbols together.
  std::unordered_map<SymbolIdx, CclsSemanticHighlightSymbol> grouped_symbols;
  if (!match.matches(file.def->path))
    return grouped_symbols;

  for (auto [sym, refcnt] : file.symbol2refcnt) {
    if (refcnt <= 0)
      continue;
    std::string_view detailed_name;
    SymbolKind parent_kind = SymbolKind::Unknown;
    SymbolKind kind = SymbolKind::Unknown;
    uint8_t storage = SC_None;
    int idx;
    // This switch statement also filters out symbols that are not highlighted.
    switch (sym.kind) {
    case Kind::Func: {
      idx = db->func_usr[sym.usr];
      const QueryFunc &func = db->funcs[idx];
      const QueryFunc::Def *def = func.anyDef();
      if (!def)
        continue; // applies to for loop
      // Don't highlight overloadable operators or implicit lambda ->
      // std::function constructor.
      std::string_view short_name = def->name(false);
      if (short_name.compare(0, 8, "operator") == 0)
        continue; // applies to for loop
      kind = def->kind;
      storage = def->storage;
      detailed_name = short_name;
      parent_kind = def->parent_kind;

      // Check whether the function name is actually there.
      // If not, do not publish the semantic highlight.
      // E.g. copy-initialization of constructors should not be highlighted
      // but we still want to keep the range for jumping to definition.
      std::string_view concise_name =
          detailed_name.substr(0, detailed_name.find('<'));
      Pos::LineNumType start_line = sym.range.start.line;
      int16_t start_col = sym.range.start.column;
      if (start_line >= wfile->index_lines.size())
        continue;
      std::string_view line = wfile->index_lines[start_line];
      sym.range.end.line = start_line;
      if (!(start_col + concise_name.size() <= line.size() &&
            line.compare(start_col, concise_name.size(), concise_name) == 0))
        continue;
      sym.range.end.column = start_col + concise_name.size();
      break;
    }
    case Kind::Type: {
      idx = db->type_usr[sym.usr];
      const QueryType &type = db->types[idx];
      for (auto &def : type.def) {
        kind = def.kind;
        detailed_name = def.detailed_name;
        if (def.spell) {
          parent_kind = def.parent_kind;
          break;
        }
      }
      break;
    }
    case Kind::Var: {
      idx = db->var_usr[sym.usr];
      const QueryVar &var = db->vars[idx];
      for (auto &def : var.def) {
        kind = def.kind;
        storage = def.storage;
        detailed_name = def.detailed_name;
        if (def.spell) {
          parent_kind = def.parent_kind;
          break;
        }
      }
      break;
    }
    default:
      continue; // applies to for loop
    }

    if (std::optional<lsRange> loc = getLsRange(wfile, sym.range)) {
      auto it = grouped_symbols.find(sym);
      if (it != grouped_symbols.end()) {
        it->second.lsOccurs.push_back({*loc, sym.role});
      } else {
        CclsSemanticHighlightSymbol symbol;
        symbol.id = idx;
        symbol.parentKind = parent_kind;
        symbol.kind = kind;
        symbol.storage = storage;
        symbol.lsOccurs.push_back({*loc, sym.role});
        grouped_symbols[sym] = symbol;
      }
    }
  }

  // Make ranges non-overlapping using a scan line algorithm.
  std::vector<ScanLineEvent> events;
  int id = 0;
  for (auto &entry : grouped_symbols) {
    CclsSemanticHighlightSymbol &symbol = entry.second;
    for (auto &occur : symbol.lsOccurs) {
      // For ranges sharing the same start point, the one with leftmost end
      // point comes first.
      events.push_back({occur.range.start, occur.range.end, id, occur.role, &symbol});
      // For ranges sharing the same end point, their relative order does not
      // matter, therefore we arbitrarily assign occur.range.end to them. We use
      // negative id to indicate a deletion event.
      events.push_back({occur.range.end, occur.range.end, ~id, occur.role, &symbol});
      id++;
    }
    symbol.lsOccurs.clear();
  }
  std::sort(events.begin(), events.end());

  std::vector<uint8_t> deleted(id, 0);
  int top = 0;
  for (size_t i = 0; i < events.size(); i++) {
    while (top && deleted[events[top - 1].id])
      top--;
    // Order [a, b0) after [a, b1) if b0 < b1. The range comes later overrides
    // the ealier. The order of [a0, b) [a1, b) does not matter.
    // The order of [a, b) [b, c) does not as long as we do not emit empty
    // ranges.
    // Attribute range [events[i-1].pos, events[i].pos) to events[top-1].symbol
    // .
    if (top && !(events[i - 1].pos == events[i].pos))
      events[top - 1].symbol->lsOccurs.push_back({{events[i - 1].pos, events[i].pos}, events[i].role});
    if (events[i].id >= 0)
      events[top++] = events[i];
    else
      deleted[~events[i].id] = 1;
  }
  return grouped_symbols;
}

void emitSemanticHighlight(DB *db, WorkingFile *wfile, QueryFile &file) {
  // Disable $ccls/publishSemanticHighlight if semantic tokens support is
  // enabled or the file is too large.
  if (g_config->client.semanticTokensRefresh || wfile->buffer_content.size() > g_config->highlight.largeFileSize)
    return;
  auto grouped_symbols = computeSemanticTokens(db, wfile, file);

  CclsSemanticHighlight params;
  params.uri = DocumentUri::fromPath(wfile->filename);
  // Transform lsRange into pair<int, int> (offset pairs)
  {
    std::vector<std::pair<Occur, CclsSemanticHighlightSymbol *>> scratch;
    for (auto &entry : grouped_symbols) {
      for (auto &occur : entry.second.lsOccurs)
        scratch.push_back({occur, &entry.second});
      entry.second.lsOccurs.clear();
    }
    std::sort(scratch.begin(), scratch.end(), [](auto &l, auto &r) { return l.first.range < r.first.range; });
    const auto &buf = wfile->buffer_content;
    int l = 0, c = 0, i = 0, p = 0;
    auto mov = [&](int line, int col) {
      if (l < line)
        c = 0;
      for (; l < line && i < buf.size(); i++) {
        if (buf[i] == '\n')
          l++;
        if (uint8_t(buf[i]) < 128 || 192 <= uint8_t(buf[i]))
          p++;
      }
      if (l < line)
        return true;
      for (; c < col && i < buf.size() && buf[i] != '\n'; c++)
        if (p++, uint8_t(buf[i++]) >= 128)
          // Skip 0b10xxxxxx
          while (i < buf.size() && uint8_t(buf[i]) >= 128 && uint8_t(buf[i]) < 192)
            i++;
      return c < col;
    };
    for (auto &entry : scratch) {
      lsRange &r = entry.first.range;
      if (mov(r.start.line, r.start.character))
        continue;
      int beg = p;
      if (mov(r.end.line, r.end.character))
        continue;
      entry.second->ranges.emplace_back(beg, p);
    }
  }

  for (auto &entry : grouped_symbols)
    if (entry.second.ranges.size() || entry.second.lsOccurs.size())
      params.symbols.push_back(std::move(entry.second));
  pipeline::notify("$ccls/publishSemanticHighlight", params);
}

void MessageHandler::textDocument_semanticTokensFull(TextDocumentParam &param, ReplyOnce &reply) {
  SemanticTokensRangeParams parameters{param.textDocument, lsRange{{0, 0}, {UINT16_MAX, INT16_MAX}}};
  textDocument_semanticTokensRange(parameters, reply);
}

void MessageHandler::textDocument_semanticTokensRange(SemanticTokensRangeParams &param, ReplyOnce &reply) {
  int file_id;
  auto [file, wf] = findOrFail(param.textDocument.uri.getPath(), reply, &file_id);
  if (!wf)
    return;

  auto grouped_symbols = computeSemanticTokens(db, wf, *file);
  std::vector<std::pair<Occur, CclsSemanticHighlightSymbol *>> scratch;
  for (auto &entry : grouped_symbols) {
    for (auto &occur : entry.second.lsOccurs)
      scratch.emplace_back(occur, &entry.second);
    entry.second.lsOccurs.clear();
  }
  std::sort(scratch.begin(), scratch.end(), [](auto &l, auto &r) { return l.first.range < r.first.range; });

  SemanticTokensPartialResult result;
  int line = 0, column = 0;
  for (auto &entry : scratch) {
    lsRange &r = entry.first.range;
    CclsSemanticHighlightSymbol &symbol = *entry.second;
    if (r.start.line != line)
      column = 0;
    result.data.push_back(r.start.line - line);
    line = r.start.line;
    result.data.push_back(r.start.character - column);
    column = r.start.character;
    result.data.push_back(r.end.character - r.start.character);

    int tokenType = (int)symbol.kind, modifier = 0;
    if (tokenType == (int)SymbolKind::StaticMethod) {
      tokenType = (int)SymbolKind::Method;
      modifier |= 1 << (int)TokenModifier::Static;
    } else if (tokenType >= (int)SymbolKind::FirstExtension) {
      tokenType += (int)SymbolKind::FirstNonStandard - (int)SymbolKind::FirstExtension;
    }

    // Set modifiers.
    if (entry.first.role & Role::Declaration)
      modifier |= 1 << (int)TokenModifier::Declaration;
    if (entry.first.role & Role::Definition)
      modifier |= 1 << (int)TokenModifier::Definition;
    if (entry.first.role & Role::Read)
      modifier |= 1 << (int)TokenModifier::Read;
    if (entry.first.role & Role::Write)
      modifier |= 1 << (int)TokenModifier::Write;
    if (symbol.storage == SC_Static)
      modifier |= 1 << (int)TokenModifier::Static;

    if (llvm::is_contained({SymbolKind::Constructor, SymbolKind::Field, SymbolKind::Method, SymbolKind::StaticMethod},
                           symbol.kind))
      modifier |= 1 << (int)TokenModifier::ClassScope;
    else if (llvm::is_contained({SymbolKind::File, SymbolKind::Namespace}, symbol.parentKind))
      modifier |= 1 << (int)TokenModifier::NamespaceScope;
    else if (llvm::is_contained(
                 {SymbolKind::Constructor, SymbolKind::Function, SymbolKind::Method, SymbolKind::StaticMethod},
                 symbol.parentKind))
      modifier |= 1 << (int)TokenModifier::FunctionScope;

    // Rainbow semantic tokens
    static_assert((int)TokenModifier::Id0 + 20 < 31);
    if (int rainbow = g_config->highlight.rainbow)
      modifier |= 1 << ((int)TokenModifier::Id0 + symbol.id % std::min(rainbow, 20));

    result.data.push_back(tokenType);
    result.data.push_back(modifier);
  }

  reply(result);
}

} // namespace ccls
