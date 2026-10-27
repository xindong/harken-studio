from __future__ import annotations

import ctypes
from contextlib import contextmanager
from functools import cache
from pathlib import Path


@cache
def api():
    dll = ctypes.WinDLL("msi.dll", use_last_error=True)
    uint = ctypes.c_uint
    text = ctypes.c_wchar_p
    pointer = ctypes.POINTER(uint)
    signatures = {
        "MsiOpenDatabaseW": [text, ctypes.c_void_p, pointer],
        "MsiDatabaseOpenViewW": [uint, text, pointer],
        "MsiViewExecute": [uint, uint],
        "MsiViewFetch": [uint, pointer],
        "MsiCreateRecord": [uint],
        "MsiRecordGetFieldCount": [uint],
        "MsiRecordSetStringW": [uint, uint, text],
        "MsiRecordSetInteger": [uint, uint, ctypes.c_int],
        "MsiRecordSetStreamW": [uint, uint, text],
        "MsiRecordGetStringW": [uint, uint, text, pointer],
        "MsiDatabaseCommit": [uint],
        "MsiDatabaseImportW": [uint, text, text],
        "MsiCloseHandle": [uint],
        "MsiGetSummaryInformationW": [uint, text, uint, pointer],
        "MsiSummaryInfoSetPropertyW": [uint, uint, uint, ctypes.c_int, ctypes.c_void_p, text],
        "MsiSummaryInfoGetPropertyW": [uint, uint, pointer, ctypes.POINTER(ctypes.c_int), ctypes.c_void_p, text, pointer],
        "MsiSummaryInfoPersist": [uint],
        "MsiGetFileVersionW": [text, text, pointer, text, pointer],
        "MsiGetFileHashW": [text, uint, ctypes.c_void_p],
        "MsiGetLastErrorRecord": [],
        "MsiFormatRecordW": [uint, uint, text, pointer],
    }
    for name, arguments in signatures.items():
        function = getattr(dll, name)
        function.argtypes = arguments
        function.restype = uint
    return dll


def checked(code):
    if not code:
        return
    detail = ""
    record = api().MsiGetLastErrorRecord()
    if record:
        try:
            length = ctypes.c_uint(4096)
            buffer = ctypes.create_unicode_buffer(length.value)
            if api().MsiFormatRecordW(0, record, buffer, ctypes.byref(length)) == 0:
                detail = buffer.value
        finally:
            api().MsiCloseHandle(record)
    raise RuntimeError(f"Windows Installer error {code}: {detail}")


@contextmanager
def handle(value):
    try:
        yield value
    finally:
        api().MsiCloseHandle(value)


class Database:
    def __init__(self, path: Path, mode: int = 3):
        value = ctypes.c_uint()
        checked(api().MsiOpenDatabaseW(str(path.resolve()), mode, ctypes.byref(value)))
        self.value = value.value

    def __enter__(self):
        return self

    def __exit__(self, *_):
        api().MsiCloseHandle(self.value)

    @contextmanager
    def view(self, sql):
        value = ctypes.c_uint()
        checked(api().MsiDatabaseOpenViewW(self.value, sql, ctypes.byref(value)))
        with handle(value.value):
            yield value.value

    def execute(self, sql, values=()):
        with self.view(sql) as view, handle(api().MsiCreateRecord(len(values))) as record:
            for index, value in enumerate(values, 1):
                if value is None:
                    continue
                if isinstance(value, Path):
                    checked(api().MsiRecordSetStreamW(record, index, str(value.resolve())))
                elif isinstance(value, int):
                    checked(api().MsiRecordSetInteger(record, index, value))
                else:
                    checked(api().MsiRecordSetStringW(record, index, value))
            checked(api().MsiViewExecute(view, record))

    def insert(self, table, rows):
        columns = self.query(f"SELECT `Name` FROM `_Columns` WHERE `Table`='{table}' ORDER BY `Number`")
        names = ",".join(f"`{column[0]}`" for column in columns)
        for row in rows:
            placeholders = ",".join("?" for _ in row)
            self.execute(f"INSERT INTO `{table}` ({names}) VALUES ({placeholders})", row)

    def query(self, sql):
        result = []
        with self.view(sql) as view:
            checked(api().MsiViewExecute(view, 0))
            while True:
                value = ctypes.c_uint()
                code = api().MsiViewFetch(view, ctypes.byref(value))
                if code == 259:
                    break
                checked(code)
                with handle(value.value) as record:
                    row = []
                    for index in range(1, api().MsiRecordGetFieldCount(record) + 1):
                        length = ctypes.c_uint()
                        code = api().MsiRecordGetStringW(record, index, None, ctypes.byref(length))
                        if code not in (0, 234):
                            checked(code)
                        length.value += 1
                        buffer = ctypes.create_unicode_buffer(length.value)
                        checked(api().MsiRecordGetStringW(record, index, buffer, ctypes.byref(length)))
                        row.append(buffer.value)
                    result.append(row)
        return result

    def summary(self, properties):
        value = ctypes.c_uint()
        checked(api().MsiGetSummaryInformationW(self.value, None, len(properties), ctypes.byref(value)))
        with handle(value.value) as summary:
            for key, value in properties.items():
                integer = isinstance(value, int)
                checked(api().MsiSummaryInfoSetPropertyW(
                    summary, key, (2 if key == 1 else 3) if integer else 30,
                    value if integer else 0, None, None if integer else value))
            checked(api().MsiSummaryInfoPersist(summary))

    def commit(self):
        checked(api().MsiDatabaseCommit(self.value))

    def summary_value(self, key):
        value = ctypes.c_uint()
        checked(api().MsiGetSummaryInformationW(self.value, None, 0, ctypes.byref(value)))
        with handle(value.value) as summary:
            kind, integer, length = ctypes.c_uint(), ctypes.c_int(), ctypes.c_uint()
            code = api().MsiSummaryInfoGetPropertyW(summary, key, ctypes.byref(kind),
                ctypes.byref(integer), None, None, ctypes.byref(length))
            if code not in (0, 234):
                checked(code)
            if kind.value in (2, 3):
                return integer.value
            length.value += 1
            buffer = ctypes.create_unicode_buffer(length.value)
            checked(api().MsiSummaryInfoGetPropertyW(summary, key, ctypes.byref(kind),
                ctypes.byref(integer), None, buffer, ctypes.byref(length)))
            return buffer.value

    def import_table(self, path: Path):
        checked(api().MsiDatabaseImportW(self.value, str(path.resolve().parent), path.name))


def file_metadata(path: Path):
    version = ctypes.create_unicode_buffer(256)
    language = ctypes.create_unicode_buffer(256)
    version_length = ctypes.c_uint(256)
    language_length = ctypes.c_uint(256)
    code = api().MsiGetFileVersionW(str(path.resolve()), version, ctypes.byref(version_length),
                                  language, ctypes.byref(language_length))
    if code == 0:
        return version.value, language.value or None, None
    if code not in (1006, 1812, 1813):
        checked(code)
    data = (ctypes.c_uint32 * 5)(20, 0, 0, 0, 0)
    checked(api().MsiGetFileHashW(str(path.resolve()), 0, ctypes.byref(data)))
    return None, None, [ctypes.c_int32(value).value for value in data[1:]]
