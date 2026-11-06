# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import re
import shlex
from collections.abc import Callable


def parse_expr(expr_text: str, valid_variables: set[str]) -> ExprNode:
    """parses the simple criteria expression syntax used in
    dependency specifications.
    Returns an ExprNode instance that can be evaluated like this:

    ```
    expr = parse_expr("os=windows")
    ok = expr.eval({
        "os": "windows"
    })
    ```

    Whitespace is allowed between tokens.  The following terms
    are recognized:

    KEY = VALUE   # Evaluates to True if ctx[KEY] == VALUE
    not(EXPR)     # Evaluates to True if EXPR evaluates to False
                  # and vice versa
    all(EXPR1, EXPR2, ...) # Evaluates True if all of the supplied
                           # EXPR's also evaluate True
    any(EXPR1, EXPR2, ...) # Evaluates True if any of the supplied
                           # EXPR's also evaluate True, False if
                           # none of them evaluated true.
    """

    p = Parser(expr_text, valid_variables)
    return p.parse()


class ExprNode:
    def eval(self, ctx: dict[str, str | None]) -> bool:
        return False


class TrueExpr(ExprNode):
    def eval(self, ctx: dict[str, str | None]) -> bool:
        return True

    def __str__(self) -> str:
        return "true"


class NotExpr(ExprNode):
    def __init__(self, node: ExprNode) -> None:
        self._node: ExprNode = node

    def eval(self, ctx: dict[str, str | None]) -> bool:
        return not self._node.eval(ctx)

    def __str__(self) -> str:
        return "not(%s)" % self._node


class AllExpr(ExprNode):
    def __init__(self, nodes: list[ExprNode]) -> None:
        self._nodes: list[ExprNode] = nodes

    def eval(self, ctx: dict[str, str | None]) -> bool:
        for node in self._nodes:
            if not node.eval(ctx):
                return False
        return True

    def __str__(self) -> str:
        items: list[str] = []
        for node in self._nodes:
            items.append(str(node))
        return "all(%s)" % ",".join(items)


class AnyExpr(ExprNode):
    def __init__(self, nodes: list[ExprNode]) -> None:
        self._nodes: list[ExprNode] = nodes

    def eval(self, ctx: dict[str, str | None]) -> bool:
        for node in self._nodes:
            if node.eval(ctx):
                return True
        return False

    def __str__(self) -> str:
        items: list[str] = []
        for node in self._nodes:
            items.append(str(node))
        return "any(%s)" % ",".join(items)


class EqualExpr(ExprNode):
    def __init__(self, key: str, value: str) -> None:
        self._key: str = key
        self._value: str = value

    def eval(self, ctx: dict[str, str | None]) -> bool:
        return ctx.get(self._key) == self._value

    def __str__(self) -> str:
        return "%s=%s" % (self._key, self._value)


class Parser:
    def __init__(self, text: str, valid_variables: set[str]) -> None:
        self.text: str = text
        self.lex: shlex.shlex = shlex.shlex(text)
        self.valid_variables: set[str] = valid_variables

    def parse(self) -> ExprNode:
        expr = self.top()
        garbage = self.lex.get_token()
        if garbage != "":
            raise Exception(
                "Unexpected token %s after EqualExpr in %s" % (garbage, self.text)
            )
        return expr

    def top(self) -> ExprNode:
        name = self.ident()
        op = self.lex.get_token()

        if op == "(":
            parsers: dict[str, Callable[[], ExprNode]] = {
                "not": self.parse_not,
                "any": self.parse_any,
                "all": self.parse_all,
            }
            func = parsers.get(name)
            if not func:
                raise Exception("invalid term %s in %s" % (name, self.text))
            return func()

        if op == "=":
            # feature_<name> variables are always valid: they're declared
            # per-manifest in [features] sections, so the global validator
            # can't enforce the declaration. An undeclared feature simply
            # evaluates to "off" at runtime.
            if name not in self.valid_variables and not name.startswith("feature_"):
                raise Exception("unknown variable %r in expression" % (name,))
            # remove shell quote from value so can test things with period in them, e.g "18.04"
            token = self.lex.get_token()
            if token is None:
                raise Exception("unexpected end of expression in %s" % self.text)
            unquoted = " ".join(shlex.split(token))
            return EqualExpr(name, unquoted)

        raise Exception(
            "Unexpected token sequence '%s %s' in %s" % (name, op, self.text)
        )

    def ident(self) -> str:
        ident = self.lex.get_token()
        if ident is None or not re.match(r"[a-zA-Z][a-zA-Z0-9_]*", ident):
            raise Exception("expected identifier found %s" % ident)
        return ident

    def parse_not(self) -> NotExpr:
        node = self.top()
        expr = NotExpr(node)
        tok = self.lex.get_token()
        if tok != ")":
            raise Exception("expected ')' found %s" % tok)
        return expr

    def parse_any(self) -> AnyExpr:
        nodes: list[ExprNode] = []
        while True:
            nodes.append(self.top())
            tok = self.lex.get_token()
            if tok == ")":
                break
            if tok != ",":
                raise Exception("expected ',' or ')' but found %s" % tok)
        return AnyExpr(nodes)

    def parse_all(self) -> AllExpr:
        nodes: list[ExprNode] = []
        while True:
            nodes.append(self.top())
            tok = self.lex.get_token()
            if tok == ")":
                break
            if tok != ",":
                raise Exception("expected ',' or ')' but found %s" % tok)
        return AllExpr(nodes)
