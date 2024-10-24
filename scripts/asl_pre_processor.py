import re
from pprint import pprint

# Token types
KEYWORDS = {
    "AccessAs",
    "Acquire",
    "Add",
    "Alias",
    "And",
    "ArgX",
    "BankField",
    "Break",
    "BreakPoint",
    "Buffer",
    "Case",
    "Concatenate",
    "ConcatenateResTemplate",
    "CondRefOf",
    "Connection",
    "Continue",
    "CopyObject",
    "CreateBitField",
    "CreateByteField",
    "CreateDWordField",
    "CreateField",
    "CreateQWordField",
    "CreateWordField",
    "DataTableRegion",
    "Debug",
    "Decrement",
    "Default",
    "DefinitionBlock",
    "DerefOf",
    "Device",
    "Divide",
    "DMA",
    "DWordIO",
    "DWordMemory",
    "DWordSpace",
    "EisaId",
    "Else",
    "ElseIf",
    "EndDependentFn",
    "Event",
    "ExtendedIO",
    "ExtendedMemory",
    "ExtendedSpace",
    "External",
    "Fatal",
    "Field",
    "FindSetLeftBit",
    "FindSetRightBit",
    "FixedDMA",
    "FixedIO",
    "Fprintf",
    "FromBCD",
    "Function",
    "GpioInt",
    "GpioIo",
    "I2CSerialBusV2",
    "If",
    "Include",
    "Increment",
    "Index",
    "IndexField",
    "Interrupt",
    "IO",
    "IRQ",
    "IRQNoFlags",
    "LAnd",
    "LEqual",
    "LGreater",
    "LGreaterEqual",
    "LLess",
    "LLessEqual",
    "LNot",
    "LNotEqual",
    "Load",
    "LoadTable",
    "LocalX",
    "LOr",
    "Match",
    "Memory24",
    "Memory32",
    "Memory32Fixed",
    "Method",
    "Mid",
    "Mod",
    "Multiply",
    "Mutex",
    "Name",
    "NAnd",
    "NoOp",
    "NOr",
    "Not",
    "Notify",
    "ObjectType",
    "Offset",
    "One",
    "Ones",
    "OperationRegion",
    "Or",
    "Package",
    "PowerResource",
    "Printf",
    "Processor",
    "QWordIO",
    "QWordMemory",
    "QWordSpace",
    "RawDataBuffer",
    "RefOf",
    "Register",
    "Release",
    "Reset",
    "ResourceTemplate",
    "Return",
    "Revision",
    "Scope",
    "ShiftLeft",
    "ShiftRight",
    "Signal",
    "SizeOf",
    "Sleep",
    "SPISerialbusV2",
    "Stall",
    "StartDependentFn",
    "StartDependentFnNoPri",
    "Store",
    "Subtract",
    "Switch",
    "ThermalZone",
    "Timer",
    "ToBCD",
    "ToBuffer",
    "ToDecimalString",
    "ToHexString",
    "ToInteger",
    "ToPLD",
    "ToString",
    "ToUUID",
    "Unicode",
    "UARTSerialBusV2",
    "VendorLong",
    "VendorShort",
    "Wait",
    "While",
    "WordBusNumber",
    "WordIO",
    "WordSpace",
    "Xor",
    "Zero"
}

class AslParser:

    def __init__(self, text: str):
        self._text = text
        self._idx = 0

    def _has(self):
        return self._idx < len(self._text)

    def _current(self):
        return self._text[self._idx]

    def _peek(self, i=1):
        return self._text[self._idx + i]

    def _next(self):
        self._idx += 1
        return self._current()

    def _skip_spaces(self):
        while self._has() and self._current() in '\r\n\t ':
            self._idx += 1

    def _expect_token(self, token):
        self._skip_spaces()
        self._expect(token)
        self._skip_spaces()

    def _expect(self, token):
        idx = self._idx
        for c in token:
            assert self._current() == c, f"Wanted {token}, got {self._text[idx:self._idx+1]}"
            self._next()

    def _check_token(self, token):
        self._skip_spaces()
        a = self._check(token)
        self._skip_spaces()
        return a

    def _check(self, token):
        idx = self._idx
        for c in token:
            if self._text[idx] != c:
                return False
            idx += 1
        self._idx = idx
        return True

    def _hex_digit_char(self):
        c = self._current()
        if c in '0123456789ABCDEFabcdef':
            self._next()
            return int(c, 16)
        else:
            return None

    def _octal_digit_char(self):
        c = self._current()
        if c in '01234567':
            self._next()
            return int(self._current())
        else:
            return None

    def _decimal_const(self):
        if self._current() not in '123456789':
            return None

        val = int(self._current())
        while self._next() in '0123456789':
            val *= 10
            val += int(self._current())

        return val

    def _octal_const(self):
        if self._current() == '0':
            return None

        val = 0
        while self._next() in '01234567':
            val *= 8
            val += int(self._current())
        return val

    def _hex_const(self):
        if not self._check('0x') and not self._check('0X'):
            return None

        val = self._hex_digit_char()
        assert val is not None

        while self._next() in '0123456789ABCDEFabcdef':
            val <<= 4
            val |= int(self._current(), 16)

        return val

    def _integer(self):
        val = self._decimal_const()
        if val is not None:
            return val

        val = self._octal_const()
        if val is not None:
            return val

        val = self._hex_const()
        if val is not None:
            return val

        return None

    def _byte_const(self):
        val = self._integer()
        if val is None:
            return None
        assert 0x00 <= val <= 0xFF
        return val

    def _word_const(self):
        val = self._integer()
        if val is None:
            return None
        assert 0x0000 <= val <= 0xFFFF
        return val

    def _dword_const(self):
        val = self._integer()
        if val is None:
            return None
        assert 0x00000000 <= val <= 0xFFFFFFFF
        return val

    def _qword_const(self):
        val = self._integer()
        if val is None:
            return None
        assert 0x0000000000000000 <= val <= 0xFFFFFFFFFFFFFFFF
        return val

    def _utf8char(self):
        a = ord(self._current())
        ranges = [
            (0x01, 0x21),
            (0x23, 0x5B),
            (0x5D, 0x7F)
        ]
        for (start, end) in ranges:
            if start <= a <= end:
                self._next()
                return chr(a)
        return None

    def _hex_escape_sequence(self):
        if not self._check('\\x'):
            return None

        a = self._hex_digit_char()
        assert a is not None

        b = self._hex_digit_char()
        if b is not None:
            a <<= 4
            a |= b

        return chr(a)

    def _simple_escape_sequence(self):
        if self._check("\\'"):
            return "'"

        if self._check('\\"'):
            return '"'

        if self._check('\\a'):
            return '\a'

        if self._check('\\b'):
            return '\b'

        if self._check('\\f'):
            return '\f'

        if self._check('\\n'):
            return '\n'

        if self._check('\\r'):
            return '\r'

        if self._check('\\t'):
            return '\t'

        if self._check('\\v'):
            return '\v'

        if self._check('\\\\'):
            return '\\'

    def _octal_escape_sequence(self):
        if self._current() != '\\' or self._peek() not in '01234567':
            return None

        self._expect('\\')
        val = self._octal_digit_char()
        assert val is not None

        n = self._octal_digit_char()
        if n is not None:
            val *= 8
            val += n

            n = self._octal_digit_char()
            if n is not None:
                val *= 8
                val += n

        return chr(val)

    def _escape_sequence(self):
        a = self._simple_escape_sequence()
        if a is not None:
            return a

        a = self._octal_escape_sequence()
        if a is not None:
            return a

        a = self._hex_escape_sequence()
        if a is not None:
            return a

        return None

    def _utf8_char_list(self):
        s = ''
        while True:
            a = self._escape_sequence()
            if a is None:
                a = self._utf8char()
                if a is None:
                    break

            s += a
        return s

    def _string(self):
        self._expect('"')
        s = self._utf8_char_list()
        self._expect('"')
        return s

    def _add_term(self):
        if not self._check_token('Add'):
            return None

        self._expect_token('(')
        p1 = self._term_arg()
        self._expect_token(',')
        p2 = self._term_arg()
        self._expect_token(',')
        result = self._target()
        self._expect_token(')')

        return {'type': 'Add', 'arg1': p1, 'arg2': p2, 'result': result}

    def _and_term(self):
        if not self._check_token('And'):
            return None

        self._expect_token('(')
        p1 = self._term_arg()
        self._expect_token(',')
        p2 = self._term_arg()
        self._expect_token(',')
        result = self._target()
        self._expect_token(')')

        return {'type': 'Add', 'arg1': p1, 'arg2': p2, 'result': result}

    def _expression_opcode(self):
        if (t := self._add_term()) is not None:
            return t
        if (t := self._and_term()) is not None:
            return t
        return None

    def _arg_term(self):
        for i in range(7):
            if self._check(f'Arg{i}'):
                return {'type': f'Arg{i}'}
        return None

    def _local_term(self):
        for i in range(8):
            if self._check(f'Local{i}'):
                return {'type': f'Local{i}'}
        return None

    def _super_name(self):
        # TODO: name_string
        if (a := self._arg_term()) is not None:
            return a
        if (a := self._local_term()) is not None:
            return a
        # TODO: debug_term
        # TODO: reference_type_opcode
        # TODO: MethodInvocationTerm
        return None

    def _term_arg(self):
        if (a := self._expression_opcode()) is not None:
            return a
        # TODO: data object
        if (a := self._arg_term()) is not None:
            return a
        if (a := self._local_term()) is not None:
            return a
        # TODO: name string
        # TODO: symbolic expression
        assert False

    def _target(self):
        return self._super_name()

    def _term(self):
        # TODO: object
        # TODO: statement opcode
        if (a := self._expression_opcode()) is not None:
            return a
        # TODO: symbolic expression
        return None

    def _term_list(self):
        t = []
        while True:
            # skip semi-colons
            while self._check(';'):
                continue

            term = self._term()
            if term is None:
                break

            t.append(term)

        return t

    def _definition_block_term(self):
        self._expect_token('DefinitionBlock')
        self._expect_token('(')
        aml_file_name = self._string()
        self._expect_token(',')
        table_signature = self._string()
        self._expect_token(',')
        compliance_revision = self._byte_const()
        self._expect_token(',')
        oemid = self._string()
        self._expect_token(',')
        table_id = self._string()
        self._expect_token(',')
        oem_revision = self._dword_const()
        self._expect_token(')')
        self._expect_token('{')
        terms = self._term_list()
        self._expect_token('}')
        return {
            'type': 'DefinitionBlock',
            'AMLFileName': aml_file_name,
            'TableSignature': table_signature,
            'ComplianceRevision': compliance_revision,
            'OEMID': oemid,
            'TableID': table_id,
            'OEMRevision': oem_revision,
            'Terms': terms
        }


    def _definition_block_list(self):
        defs = []
        while self._has():
            defs.append(self._definition_block_term())
        return defs

    def asl_code(self):
        return self._definition_block_list()


parser = AslParser("""
DefinitionBlock ("x.aml", "DSDT", 2, "uTEST", "TESTTABL", 0xF0F0F0F0)
{
    Add(Local0, Local1, Local2)
}
""")

pprint(parser.asl_code())