import argparse
import ast
import enum
import inspect
import os
import struct
import tempfile
from dataclasses import dataclass

from sympy import false
from sympy.physics.units import current

ACPI_TABLE_HEADER = struct.Struct('<4sIBB6s8sI4sI')


class Compiler:

    def __init__(self, mod_name = 'DMOD'):
        self._namespace = {}
        self.current_node = self._namespace
        self._node_stack = []

        self.buffer = bytearray()

        self._name_idx = 0

        self._module_pkg_length = None
        self._init_module(mod_name)

    def _gen_name(self, name, typ):
        if name == 'main':
            acpi_name = 'MAIN'
        else:
            acpi_name = f'U{self._name_idx:03x}'
            self._name_idx += 1
        self.current_node[name] = (acpi_name, typ)

    def _init_module(self, mod_name):
        if mod_name is None:
            return

        # emit a device to enclose the entire program
        self.emit_byte(0x5B)
        self.emit_byte(0x82)
        self._module_pkg_length = self.start_pkg_length()
        self.emit_name(mod_name)

    def resolve_path(self, longname):
        return self.current_node[longname]

    def resolve_type_annotation(self, ann):
        assert ann is not None, "Missing type annotation"
        if isinstance(ann, ast.Name):
            if ann.id == 'int':
                return int
            elif ann.id == 'bytes':
                return bytes
            elif ann.id == 'str':
                return str
            else:
                assert False, ann.id
        else:
            assert False, ast.dump(ann)

    def emit_byte(self, value, idx=None):
        value &= 0xFF
        if idx is None:
            self.buffer.append(value)
        else:
            self.buffer.insert(idx, value)

    def start_pkg_length(self):
        start = len(self.buffer)
        def close_pkg_length():
            end = len(self.buffer)
            l = end - start
            if l + 1 <= 63:
                l += 1
                self.buffer.insert(start, l)
            elif l + 2 <= 0xFF + 16:
                l += 2
                self.buffer.insert(start, 1 << 6 | (l & 0xF))
                self.buffer.insert(start + 1, (l >> 4) & 0xFF)
            elif l + 3 <= 0xFFFF + 16:
                l += 3
                self.buffer.insert(start, 2 << 6 | (l & 0xF))
                self.buffer.insert(start + 1, (l >> 4) & 0xFF)
                self.buffer.insert(start + 2, (l >> 12) & 0xFF)
            elif l + 4 <= 0xFFFFFF + 16:
                l += 4
                self.buffer.insert(start, 3 << 6 | (l & 0xF))
                self.buffer.insert(start + 1, (l >> 4) & 0xFF)
                self.buffer.insert(start + 2, (l >> 12) & 0xFF)
                self.buffer.insert(start + 3, (l >> 20) & 0xFF)
            else:
                assert False
            pass
        return close_pkg_length

    def emit_name(self, s):
        for i in s:
            assert i in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_^\\', s
            self.emit_byte(ord(i))

    def emit_const(self, val):
        val &= 0xFFFFFFFFFFFFFFFF
        if val == 0:
            self.emit_byte(0)
        elif val == 1:
            self.emit_byte(1)
        elif val == 0xFFFFFFFFFFFFFFFF:
            self.emit_byte(0xFF)
        elif val <= 0xFF:
            self.emit_byte(0x0A)
            self.emit_byte((val >> 0) & 0xFF)
        elif val <= 0xFFFF:
            self.emit_byte(0x0B)
            self.emit_byte((val >> 0) & 0xFF)
            self.emit_byte((val >> 8) & 0xFF)
        elif val <= 0xFFFFFFFF:
            self.emit_byte(0x0C)
            self.emit_byte((val >> 0) & 0xFF)
            self.emit_byte((val >> 8) & 0xFF)
            self.emit_byte((val >> 16) & 0xFF)
            self.emit_byte((val >> 24) & 0xFF)
        else:
            self.emit_byte(0x0E)
            self.emit_byte((val >> 0) & 0xFF)
            self.emit_byte((val >> 8) & 0xFF)
            self.emit_byte((val >> 16) & 0xFF)
            self.emit_byte((val >> 24) & 0xFF)
            self.emit_byte((val >> 32) & 0xFF)
            self.emit_byte((val >> 40) & 0xFF)
            self.emit_byte((val >> 48) & 0xFF)
            self.emit_byte((val >> 56) & 0xFF)

    def _push_node(self, name):
        new_node = self.current_node.setdefault(name, {})
        self._node_stack.append(self.current_node)
        self.current_node = new_node

    def _pop_node(self, name):
        self.current_node = self._node_stack.pop()

    def add_module(self, source, filename='<unknown>'):
        module = ast.parse(source, filename)

        # first build the namespace we would need
        for node in module.body:
            if isinstance(node, ast.FunctionDef):
                if node.returns is not None:
                    typ = self.resolve_type_annotation(node.returns)
                else:
                    typ = None
                self._gen_name(node.name, typ)
            else:
                assert False, ast.dump(node, indent=4)

        # now we can go over and generate all the methods
        for node in module.body:
            if isinstance(node, ast.FunctionDef):
                c = MethodCompiler(self, node)
                c.compile()
            else:
                assert False, ast.dump(node, indent=4)

    def pack(self):
        # close the module package
        if self._module_pkg_length is not None:
            self._module_pkg_length()

        # now emit the table header
        table = ACPI_TABLE_HEADER.pack(
            b'SSDT',
            ACPI_TABLE_HEADER.size + len(self.buffer),
            2,
            0,
            b'uACPI ',
            b'uACPI-OS',
            1,
            b'UOSC',
            0
        ) + self.buffer

        # TODO: calculate the checksum

        return table


class MethodCompiler:
    def __init__(self, compiler: Compiler, func: ast.FunctionDef):
        self._compiler = compiler
        self._func = func

        self._args = {}
        self._locals = {}

    def _resolve_type_annotation(self, ann):
        return self._compiler.resolve_type_annotation(ann)

    def _get_idx(self):
        return len(self._compiler.buffer)

    def _lookup_name(self, name):
        return self._compiler.current_node[name][0]

    def _emit_byte(self, val, idx=None):
        self._compiler.emit_byte(val, idx)

    def _start_pkg_length(self):
        return self._compiler.start_pkg_length()

    def _emit_name(self, name):
        self._compiler.emit_name(name)

    def _emit_const(self, val):
        self._compiler.emit_const(val)

    def _emit_expression(self, expr):
        if isinstance(expr, ast.Compare):
            assert len(expr.ops) == len(expr.comparators)
            assert len(expr.comparators) == 1

            # emit the correct opcode
            if isinstance(expr.ops[0], ast.And):
                self._emit_byte(0x90)
            elif isinstance(expr.ops[0], ast.Or):
                self._emit_byte(0x91)
            elif isinstance(expr.ops[0], ast.NotEq):
                self._emit_byte(0x92)
                self._emit_byte(0x93)
            elif isinstance(expr.ops[0], ast.LtE):
                self._emit_byte(0x92)
                self._emit_byte(0x94)
            elif isinstance(expr.ops[0], ast.GtE):
                self._emit_byte(0x92)
                self._emit_byte(0x95)
            elif isinstance(expr.ops[0], ast.Eq):
                self._emit_byte(0x93)
            elif isinstance(expr.ops[0], ast.Gt):
                self._emit_byte(0x94)
            elif isinstance(expr.ops[0], ast.Lt):
                self._emit_byte(0x95)
            else:
                assert False, expr.ops[0]

            # emit the full expression
            self._emit_expression(expr.left)
            self._emit_expression(expr.comparators[0])

            return int

        elif isinstance(expr, ast.Name):
            if expr.id in self._args:
                idx, typ = self._args[expr.id]
                self._emit_byte(0x68 + idx)
                return typ

            elif expr.id in self._locals:
                idx, typ = self._locals[expr.id]
                self._emit_byte(0x60 + idx)
                return typ

            else:
                assert False, ast.dump(expr)

        elif isinstance(expr, ast.Constant):
            if isinstance(expr.value, int):
                self._emit_const(expr.value)
                return int
            elif isinstance(expr.value, str):
                self._emit_byte(0x0D)
                for i in expr.value.encode('utf-8'):
                    self._emit_byte(i)
                self._emit_byte(0)
                return str

            else:
                assert False, ast.dump(expr)


        elif isinstance(expr, ast.Call):
            assert isinstance(expr.func, ast.Name)
            ret_typ = None

            if expr.func.id == 'print':
                if len(expr.args) == 1:
                    idx = self._get_idx()
                    typ = self._emit_expression(expr.args[0])

                    if typ == int:
                        # use ToDecimalString and its target to store
                        self._emit_byte(0x97, idx)
                    else:
                        # use a normal store
                        self._emit_byte(0x70)

                    # add the debug object
                    self._emit_byte(0x5B)
                    self._emit_byte(0x31)

                else:
                    # add spaces, attempt to merge with existing strings
                    # when possible
                    args = [expr.args[0]]
                    for arg in expr.args[1:]:
                        if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                            args.append(ast.Constant(' ' + arg.value))
                        elif isinstance(args[-1], ast.Constant) and isinstance(args[-1].value, str):
                            args[-1] = ast.Constant(args[-1].value + ' ')
                            args.append(arg)
                        else:
                            args.append(ast.Constant(' '))
                            args.append(arg)

                    for i, arg in enumerate(args):
                        # emit the concat if not the final one
                        if i + 1 < len(args):
                            self._emit_byte(0x73)

                        # emit the argument
                        idx = self._get_idx()
                        typ = self._emit_expression(arg)
                        if typ == int:
                            self._emit_byte(0x97, idx)
                            self._emit_byte(0)

                    # all the targets except for the very last one
                    # need to be nulled
                    for _ in range(len(args) - 2):
                        self._emit_byte(0)

                    # and the last target can be Debug
                    self._emit_byte(0x5B)
                    self._emit_byte(0x31)

            else:
                acpi_name, ret_typ = self._compiler.resolve_path(expr.func.id)
                self._emit_name(acpi_name)

                for arg in expr.args:
                    typ = self._emit_expression(arg)
                    # TODO: type check

            return ret_typ

        elif isinstance(expr, ast.BinOp):
            op_idx = self._get_idx()
            left = self._emit_expression(expr.left)
            right = self._emit_expression(expr.right)
            self._emit_byte(0)

            if isinstance(expr.op, ast.Add):
                if left == int and right == int:
                    # integer addition
                    self._emit_byte(0x72, op_idx)
                elif left in [str, bytes, bytearray] or right in [str, bytes, bytearray]:
                    # string or bytes
                    self._emit_byte(0x73, op_idx)
                else:
                    assert False, "invalid types"

            elif isinstance(expr.op, ast.Sub):
                assert left == int and right == int
                self._emit_byte(0x74, op_idx)

            elif isinstance(expr.op, ast.Mult):
                assert left == int and right == int
                self._emit_byte(0x77, op_idx)

            elif isinstance(expr.op, ast.FloorDiv):
                assert left == int and right == int
                self._emit_byte(0x78, op_idx)
                self._emit_byte(0) # Quotient, the other is the Remainder

            elif isinstance(expr.op, ast.LShift):
                assert left == int and right == int
                self._emit_byte(0x79, op_idx)

            elif isinstance(expr.op, ast.RShift):
                assert left == int and right == int
                self._emit_byte(0x7A, op_idx)

            elif isinstance(expr.op, ast.BitAnd):
                assert left == int and right == int
                self._emit_byte(0x7B, op_idx)

            elif isinstance(expr.op, ast.BitOr):
                assert left == int and right == int
                self._emit_byte(0x7D, op_idx)

            elif isinstance(expr.op, ast.BitXor):
                assert left == int and right == int
                self._emit_byte(0x7F, op_idx)

            elif isinstance(expr.op, ast.Mod):
                assert left == int and right == int
                self._emit_byte(0x85, op_idx)

            else:
                assert False, expr.op

            return left

        elif isinstance(expr, ast.UnaryOp):
            if isinstance(expr.op, ast.Invert):
                self._emit_byte(0x80)
            elif isinstance(expr.op, ast.Not):
                self._emit_byte(0x92)
            else:
                assert False, ast.dump(expr)

            return self._emit_expression(expr.operand)

        else:
            assert False, ast.dump(expr, indent=4)

    def _emit_statement(self, stmt):
        if isinstance(stmt, ast.Pass):
            pass

        elif isinstance(stmt, ast.If):
            self._emit_byte(0xA0)
            if_pkg = self._start_pkg_length()
            self._emit_expression(stmt.test)
            self._emit_statements(stmt.body)
            if_pkg()

            if len(stmt.orelse) > 0:
                self._emit_byte(0xA1)
                else_pkg = self._start_pkg_length()
                self._emit_statements(stmt.orelse)
                else_pkg()

        elif isinstance(stmt, ast.Return):
            self._emit_byte(0xA4)
            typ = self._emit_expression(stmt.value)
            assert typ == self._compiler.resolve_path(self._func.name)[1]

        elif isinstance(stmt, ast.Assign) or isinstance(stmt, ast.AnnAssign):
            idx = self._get_idx()
            value_type = self._emit_expression(stmt.value)

            if isinstance(stmt, ast.Assign):
                assert len(stmt.targets) == 1
                target = stmt.targets[0]
            else:
                target = stmt.target

            if isinstance(target, ast.Name):
                if target.id in self._locals:
                    if self._locals[target.id][1] is not None:
                        assert self._locals[target.id][1] == value_type, (self._locals[target.id][1], value_type)
                    else:
                        self._locals[target.id][1] = value_type

                if isinstance(stmt, ast.AnnAssign):
                    typ = self._resolve_type_annotation(stmt.annotation)
                    assert typ == value_type, (typ, value_type)

            if isinstance(stmt.value, ast.BinOp):
                # replace the last operations target with our target
                self._compiler.buffer.pop()

            else:
                # use CopyObject to store the result
                self._emit_byte(0x9D, idx)

            # and now emit the target
            target_type = self._emit_expression(target)

            # make sure the types match
            assert target_type == value_type, target_type == value_type

        elif isinstance(stmt, ast.AugAssign):
            # convert assignment with binop
            self._emit_statement(ast.Assign(
                targets=[stmt.target],
                value=ast.BinOp(
                    left=stmt.target,
                    op=stmt.op,
                    right=stmt.value
                )
            ))

        elif isinstance(stmt, ast.While):
            assert len(stmt.orelse) == 0

            self._emit_byte(0xA2)
            while_pkg = self._start_pkg_length()
            self._emit_expression(stmt.test)
            self._emit_statements(stmt.body)
            while_pkg()

        elif isinstance(stmt, ast.Expr):
            self._emit_expression(stmt.value)

        else:
            assert False, ast.dump(stmt, indent=4)

    def _emit_statements(self, stmts):
        for stmt in stmts:
            self._emit_statement(stmt)

    def compile(self):
        acpi_name = self._lookup_name(self._func.name)

        # validate argument counts
        assert len(self._func.args.posonlyargs) == 0
        assert len(self._func.args.args) <= 7
        assert self._func.args.vararg is None
        assert len(self._func.args.kwonlyargs) == 0
        assert len(self._func.args.kw_defaults) == 0
        assert self._func.args.kwarg is None
        assert len(self._func.args.defaults) == 0

        # emit the method header
        self._emit_byte(0x14)
        method_pkg = self._start_pkg_length()
        self._emit_name(acpi_name)
        self._emit_byte(len(self._func.args.args))

        # find all the args
        for arg in self._func.args.args:
            self._args[arg.arg] = (len(self._args), self._resolve_type_annotation(arg.annotation))

        # find all the locals
        for node in ast.walk(self._func):
            if isinstance(node, ast.Assign):
                for target in node.targets:
                    if isinstance(target, ast.Name):
                        assert isinstance(target.ctx, ast.Store)
                        self._locals.setdefault(target.id, [len(self._locals), None])
            elif isinstance(node, ast.AnnAssign):
                assert isinstance(node.target, ast.Name)
                assert isinstance(node.target.ctx, ast.Store)
                self._locals.setdefault(node.target.id, [len(self._locals), None])

        assert len(self._locals) <= 8, "TODO: support more locals"

        # emit the body
        self._emit_statements(self._func.body)

        # finish up the method
        method_pkg()



if __name__ == '__main__':
    parser = argparse.ArgumentParser('Python to AML compiler')
    parser.add_argument('input', help='The input python file')
    parser.add_argument('output', help='The output aml file')
    parser.add_argument('--root', default=False, action='store_true', help='Should the code be defined at the ACPI namespace root')
    args = parser.parse_args()

    with open(args.input) as f:
        source = f.read()

    mod_name = 'DMOD'
    if args.root:
        mod_name = None

    c = Compiler(mod_name)
    c.add_module(source, args.input)

    with open(args.output, 'wb') as f:
        f.write(c.pack())

    # with tempfile.NamedTemporaryFile() as f:
    os.system(f'iasl -d {args.output}')
    os.system(f'cat {os.path.splitext(args.output)[0]}.dsl')
