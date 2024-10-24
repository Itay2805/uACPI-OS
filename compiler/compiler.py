import ast
import os
import struct
import textwrap
import types
import inspect
import dis
from pprint import pprint

from setuptools.command.alias import alias
from sympy.codegen.cnodes import static

ACPI_TABLE_HEADER = struct.Struct('<4sIBB6s8sI4sI')


_OPCODE_CAN_STORE = {
    0x72
}


class Table:

    def __init__(self):
        self._buffer = bytearray()
        self._id_gen = 0
        self._name_lookup = {}
        self._current_namespace = self._name_lookup
        self._namespace_stack = []

        self._args = None
        self._locals = None

        self._for_loop_next = []



    def _push_namespace(self, name):
        n = {}
        self._namespace_stack.append(self._current_namespace)
        self._current_namespace[name] = n
        self._current_namespace = n
        return n

    def _pop_namespace(self):
        self._current_namespace = self._namespace_stack.pop()

    def _gen_name(self, longname):
        name = f'U{self._id_gen:03x}'
        self._id_gen += 1
        assert name not in self._current_namespace
        self._current_namespace[longname] = name
        print(longname, name)
        return name

    def _lookup_name(self, longname):
        return self._name_lookup[longname]

    def _emit_name_string(self, s: str):
        for c in s.encode('utf-8'):
            assert c in b'0123456789\\^_ABCDEFGHIKLMNOPQRSTUVWXYZ', c
            self._buffer.append(c)

    def _emit_pkg_length(self, length: int, idx=None):
        if idx is None:
            idx = len(self._buffer)

        if (length + 1) <= 63:
            length += 1
            self._buffer.insert(idx, length)
        elif (length + 2) <= 0xFF + 16:
            length += 2
            self._buffer.insert(idx, 1 << 6 | (length & 0xF))
            self._buffer.insert(idx + 1, (length >> 4) & 0xFF)
        elif (length + 3) <= 0xFFFF + 16:
            length += 3
            self._buffer.insert(idx, 2 << 6 | (length & 0xF))
            self._buffer.insert(idx + 1, (length >> 4) & 0xFF)
            self._buffer.insert(idx + 2, (length >> 12) & 0xFF)
        elif (length + 4) <= 0xFFFFFF + 16:
            length += 4
            self._buffer.insert(idx, 3 << 6 | (length & 0xF))
            self._buffer.insert(idx + 1, (length >> 4) & 0xFF)
            self._buffer.insert(idx + 2, (length >> 12) & 0xFF)
            self._buffer.insert(idx + 3, (length >> 20) & 0xFF)
        else:
            assert False, length

    def _resolve_method(self, func):
        if isinstance(func, ast.Attribute):
            m = self._resolve_method(func.value)
            return m[func.attr]

        elif isinstance(func, ast.Name):
            return self._lookup_name(func.id)

        else:
            assert False, ast.dump(func, indent=4)

    def _emit_expression(self, node):
        if isinstance(node, ast.Compare):
            assert len(node.ops) == 1
            assert len(node.comparators) == 1

            op = {
                ast.And: (0x90,),
                ast.Or: (0x91,),
                ast.NotEq: (0x92, 0x93),
                ast.LtE: (0x92, 0x94),
                ast.GtE: (0x92, 0x95),
                ast.Eq: (0x93,),
                ast.Gt: (0x94,),
                ast.Lt: (0x95,),
            }[type(node.ops[0])]
            for o in op:
                self._buffer.append(o)

            self._emit_expression(node.left)
            self._emit_expression(node.comparators[0])

        elif isinstance(node, ast.BinOp):
            self._emit_expression(node.op)
            self._emit_expression(node.left)
            self._emit_expression(node.right)
            self._buffer.append(0)

        elif isinstance(node, ast.UnaryOp):
            self._emit_expression(node.op)
            self._emit_expression(node.operand)

        elif isinstance(node, ast.Add):
            self._buffer.append(0x72)
        elif isinstance(node, ast.Sub):
            self._buffer.append(0x74)
        elif isinstance(node, ast.Mult):
            self._buffer.append(0x77)
        elif isinstance(node, ast.FloorDiv) or isinstance(node, ast.Div): # TODO: is this what I want?
            self._buffer.append(0x78)
        elif isinstance(node, ast.Mod):
            self._buffer.append(0x85)
        elif isinstance(node, ast.BitAnd):
            self._buffer.append(0x7B)
        elif isinstance(node, ast.BitOr):
            self._buffer.append(0x7D)
        elif isinstance(node, ast.BitXor):
            self._buffer.append(0x7F)
        elif isinstance(node, ast.Not):
            self._buffer.append(0x92)

        elif isinstance(node, ast.Name):
            if node.id in self._args:
                self._buffer.append(0x68 + self._args[node.id])
            else:
                # add the local if not found yet
                if node.id not in self._locals:
                    assert len(self._locals) < 8
                    self._locals[node.id] = len(self._locals)

                self._buffer.append(0x60 + self._locals[node.id])

        elif isinstance(node, ast.Call):
            # TODO: support dynamic dispatch
            name = self._resolve_method(node.func)
            self._emit_name_string(name)
            for arg in node.args:
                self._emit_expression(arg)

        elif isinstance(node, ast.Constant):
            val = node.value & 0xFFFFFFFFFFFFFFFF
            if val == 0:
                self._buffer.append(0x00)
            elif val == 1:
                self._buffer.append(0x01)
            elif val == 0xFFFFFFFFFFFFFFFF:
                self._buffer.append(0xFF)
            elif val <= 0xFF:
                self._buffer.append(0xA)
                self._buffer.append((val >> 0) & 0xFF)
            elif val <= 0xFFFF:
                self._buffer.append(0x0B)
                self._buffer.append((val >> 0) & 0xFF)
                self._buffer.append((val >> 8) & 0xFF)
            elif val <= 0xFFFFFFFF:
                self._buffer.append(0x0C)
                self._buffer.append((val >> 0) & 0xFF)
                self._buffer.append((val >> 8) & 0xFF)
                self._buffer.append((val >> 16) & 0xFF)
                self._buffer.append((val >> 24) & 0xFF)
            else:
                self._buffer.append(0x0D)
                self._buffer.append((val >> 0) & 0xFF)
                self._buffer.append((val >> 8) & 0xFF)
                self._buffer.append((val >> 16) & 0xFF)
                self._buffer.append((val >> 24) & 0xFF)
                self._buffer.append((val >> 32) & 0xFF)
                self._buffer.append((val >> 40) & 0xFF)
                self._buffer.append((val >> 48) & 0xFF)
                self._buffer.append((val >> 52) & 0xFF)

        else:
            assert False, ast.dump(node, indent=4)

    def _emit_statement(self, node):
        if isinstance(node, ast.If):
            self._buffer.append(0xA0)
            idx = len(self._buffer)

            # emit the test expression
            self._emit_expression(node.test)

            # emit the body
            for stmt in node.body:
                self._emit_statement(stmt)

            self._emit_pkg_length(len(self._buffer) - idx, idx)

            # if we have nodes at the else then emit the else node
            if len(node.orelse) > 0:
                self._buffer.append(0xA1)

                else_idx = len(self._buffer)

                for stmt in node.orelse:
                    self._emit_statement(stmt)

                self._emit_pkg_length(len(self._buffer) - else_idx, else_idx)

        elif isinstance(node, ast.While):
            self._buffer.append(0xA2)
            idx = len(self._buffer)

            assert len(node.orelse) == 0

            self._emit_expression(node.test)
            for stmt in node.body:
                self._emit_statement(stmt)

            self._emit_pkg_length(len(self._buffer) - idx, idx)

        elif isinstance(node, ast.For):
            if isinstance(node.iter, ast.Call) and isinstance(node.iter.func, ast.Name) and node.iter.func.id == 'range':
                # initialize the index variable
                if len(node.iter.args) == 1:
                    # starts from zero
                    self._emit_statement(ast.Assign([node.target], ast.Constant(0)))

                    # check its smaller than the requested value
                    # TODO: check if has side-effects (like function call) and if so store it aside
                    #       since that is not how the python for loop behaves in that case
                    predicate = ast.Compare(node.target, [ast.Lt()], [node.iter.args[0]])

                    # increments by one
                    self._for_loop_next.append(ast.AugAssign(node.target, ast.Add(), ast.Constant(1)))

                else:
                    # starts from the first arg
                    assert len(node.iter.args) <= 3

                    # start from the first variable
                    self._emit_statement(ast.Assign([node.target], node.iter.args[0]))

                    # check its smaller than the requested value
                    # TODO: check if has side-effects (like function call) and if so store it aside
                    #       since that is not how the python for loop behaves in that case
                    predicate = ast.Compare(node.target, [ast.Lt()], [node.iter.args[1]])

                    # set the step, 1 by default
                    if len(node.iter.args) == 3:
                        self._for_loop_next.append(ast.AugAssign(node.target, ast.Add(), node.iter.args[2]))
                    else:
                        self._for_loop_next.append(ast.AugAssign(node.target, ast.Add(), ast.Constant(1)))

                # emit the while opcode
                self._buffer.append(0xA2)
                idx = len(self._buffer)

                # emit the predicate
                self._emit_expression(predicate)

                # emit the body
                stmt = None
                for stmt in node.body:
                    self._emit_statement(stmt)

                # check if the last one is something that won't go to the next
                need_next = True
                if isinstance(stmt, ast.Break) or isinstance(stmt, ast.Continue) or isinstance(stmt, ast.Return):
                    need_next = False

                # emit the next
                if need_next:
                    self._emit_statement(self._for_loop_next[-1])

                self._emit_pkg_length(len(self._buffer) - idx, idx)

            else:
                assert False, ast.dump(node, indent=4)

        elif isinstance(node, ast.Return):
            self._buffer.append(0xA4)
            self._emit_expression(node.value)

        elif isinstance(node, ast.Assign):
            if isinstance(node.value, ast.BinOp):
                # we can optimize by emitting the bin-op and
                # then storing it in the result
                self._emit_expression(node.value)
                self._buffer.pop()
            else:
                # emit a store and then the normal operations
                self._buffer.append(0x70)
                self._emit_expression(node.value)

            assert len(node.targets) == 1
            self._emit_expression(node.targets[0])

        elif isinstance(node, ast.AugAssign):
            self._emit_expression(node.op)
            self._emit_expression(node.target)
            self._emit_expression(node.value)
            self._emit_expression(node.target)

        elif isinstance(node, ast.Continue):
            # the next before the continue
            if len(self._for_loop_next):
                self._emit_statement(self._for_loop_next[-1])

            self._buffer.append(0x9F)

        elif isinstance(node, ast.Break):
            self._buffer.append(0xA5)

        else:
            assert False, ast.dump(node, indent=4)

    def _emit_method(self, node: ast.FunctionDef):
        print(ast.dump(node, indent=4))

        acpi_name = self._current_namespace[node.name]

        assert len(node.args.posonlyargs) == 0
        assert len(node.args.kwonlyargs) == 0
        assert len(node.args.kw_defaults) == 0
        assert len(node.args.defaults) == 0
        assert len(node.args.args) <= 7
        method_flags = len(node.args.args)

        self._args = {}
        for arg in node.args.args:
            self._args[arg.arg] = len(self._args)

        self._locals = {}

        # emit the header
        self._buffer.append(0x14)
        idx = len(self._buffer)
        self._emit_name_string(acpi_name)
        self._buffer.append(method_flags)

        # and now emit everything
        for stmt in node.body:
            self._emit_statement(stmt)

        # fixup the pkg length
        self._emit_pkg_length(len(self._buffer) - idx, idx)

        self._locals = None
        self._args = None

    def add(self, content):
        code = ast.parse(inspect.getsource(content))
        code = code.body
        assert len(code) == 1
        code = code[0]
        assert isinstance(code, ast.ClassDef)

        # start by creating all of the names
        self._push_namespace(code.name)
        for node in code.body:
            if isinstance(node, ast.FunctionDef):
                self._gen_name(node.name)
            else:
                assert False, ast.dump(node, indent=4)

        for node in code.body:
            if isinstance(node, ast.FunctionDef):
                self._emit_method(node)

        self._pop_namespace()

    def pack(self):
        # TODO: checksum
        return ACPI_TABLE_HEADER.pack(
            b'SSDT',
            ACPI_TABLE_HEADER.size + len(self._buffer),
            2,
            0,
            b'uACPI ',
            b'uACPI-OS',
            1,
            b'UOSC',
            0
        ) + self._buffer


class Kernel:

    @staticmethod
    def thing(x):
        a = 1
        for i in range(x):
            a *= i
        return a

    @staticmethod
    def thing2(s, e, step):
        a = 1
        for i in range(s, e, step):
            a *= i
        return a



table = Table()
table.add(Kernel)

open('test.aml', 'wb').write(table.pack())
os.system('hexdump -C test.aml')
assert os.system('iasl -d test.aml') == 0
os.system('cat test.dsl')