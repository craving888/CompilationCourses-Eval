#include "parser/ast.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace {
using K = NodeKind;
struct Recover {};
struct Parser {
    Scan input;
    Program program;
    std::size_t pos = 0;

    const Token& peek(std::size_t n = 0) const { return input.tokens[std::min(pos+n, input.tokens.size()-1)]; }
    bool at(std::string_view text) const { return peek().text == text; }
    bool end() const { return peek().kind == Token::End; }
    Token pop() { Token t = peek(); if (!end()) ++pos; return t; }
    bool take(std::string_view text) { if (!at(text)) return false; pop(); return true; }
    void error(const std::string& message) { program.errors.push_back({peek().line, message}); }
    [[noreturn]] void fail(const std::string& message) { error(message); throw Recover{}; }
    void expect(std::string_view text) { if (!take(text)) fail("expected '" + std::string(text) + "'"); }
    Token ident() { if (peek().kind != Token::Ident) fail("expected identifier"); return pop(); }
    bool function_start() const {
        return (at("int") || at("void")) && peek(1).kind == Token::Ident && peek(2).text == "(";
    }
    bool stmt_start() const {
        return at("if") || at("while") || at("return") || at("int") || at("const") ||
               at("break") || at("continue") || at("{") || at("else");
    }

    template <class... Children> Tree node(K kind, Token at_token, Children... children) {
        auto n = std::make_unique<Node>(Node{kind, std::move(at_token), {}});
        (n->children.push_back(std::move(children)), ...);
        return n;
    }

    Tree primary() {
        if (take("(")) { auto e = expr(); expect(")"); return e; }
        if (peek().kind == Token::Number) return node(K::Int, pop());
        if (peek().kind != Token::Ident) fail("expected expression");
        auto name = pop();
        if (!take("(")) return node(K::Name, name);
        auto call = node(K::Call, name);
        if (!at(")")) {
            do { call->children.push_back(expr()); } while (take(","));
        }
        expect(")");
        return call;
    }

    Tree unary() {
        if (!at("+") && !at("-") && !at("!")) return primary();
        auto op = pop();
        return node(K::Unary, op, unary());
    }

    Tree expr(std::size_t level = 0) {
        static const std::array<std::vector<std::string_view>,5> operators{{
            {"||"}, {"&&"}, {"<",">","<=",">=","==","!="}, {"+","-"}, {"*","/","%"}
        }};
        if (level == operators.size()) return unary();
        auto left = expr(level+1);
        const auto& ops = operators[level];
        while (std::find(ops.begin(),ops.end(),peek().text) != ops.end()) {
            auto op = pop();
            left = node(K::Binary, op, std::move(left), expr(level+1));
        }
        return left;
    }

    Tree block() {
        auto b = node(K::Block, peek());
        expect("{");
        while (!at("}") && !end() && !function_start()) {
            auto start = pos;
            try { b->children.push_back(stmt()); }
            catch (Recover) {
                if (pos == start && !end()) pop();
                while (!end() && !at("}") && !at(";") && !stmt_start() && !function_start()) pop();
                take(";");
            }
        }
        b->close_line = peek().line;
        if (!take("}")) error("expected '}'");
        return b;
    }

    Tree stmt() {
        auto start = peek();
        if (at("{")) return block();
        if (take(";")) return node(K::Empty,start);
        if (at("int") || at("const")) {
            bool constant = take("const");
            expect("int");
            auto name = ident();
            Tree init;
            if (take("=")) init = expr();
            else if (constant) fail("constant requires an initializer");
            expect(";");
            return node(constant ? K::Const : K::Decl, name, std::move(init));
        }
        if (take("if")) {
            if (!take("(")) error("expected '('");
            auto condition = expr();
            if (!take(")")) error("expected ')'");
            auto yes = stmt();
            Tree no;
            if (take("else")) no = stmt();
            return node(K::If,start,std::move(condition),std::move(yes),std::move(no));
        }
        if (take("while")) {
            if (!take("(")) error("expected '('");
            auto condition = expr();
            if (!take(")")) {
                error("expected ')'");
                // An assignment suffix belongs to the malformed header here.
                if (at("=")) throw Recover{};
            }
            return node(K::While,start,std::move(condition),stmt());
        }
        if (take("break") || take("continue")) {
            expect(";");
            return node(start.text == "break" ? K::Break : K::Continue, start);
        }
        if (take("return")) {
            Tree value;
            if (!at(";")) value = expr();
            expect(";");
            return node(K::Return,start,std::move(value));
        }
        if (peek().kind == Token::Ident && peek(1).text == "=") {
            auto name = pop();
            pop();
            auto value = expr();
            expect(";");
            return node(K::Assign,name,std::move(value));
        }
        auto value = expr();
        expect(";");
        return node(K::Expr,start,std::move(value));
    }

    Function function() {
        bool integer = take("int");
        if (!integer) expect("void");
        Function f{ident(), integer, {}, nullptr};
        expect("(");
        if (!at(")")) {
            do { expect("int"); f.parameters.push_back(ident()); } while (take(","));
        }
        if (!take(")")) error("expected ')'");
        f.body = block();
        return f;
    }

    Program run() {
        program.last_line = input.tokens.back().line;
        program.errors = std::move(input.errors);
        if (end() && program.errors.empty()) error("expected function definition");
        while (!end()) {
            auto start = pos;
            try { program.functions.push_back(function()); }
            catch (Recover) {
                if (pos == start) pop();
                while (!end() && !function_start()) pop();
            }
        }
        std::stable_sort(program.errors.begin(), program.errors.end(),
                         [](const auto& a, const auto& b) { return a.line < b.line; });
        return std::move(program);
    }
};
}

Program parse(Scan scanned) { return Parser{std::move(scanned), {}}.run(); }
