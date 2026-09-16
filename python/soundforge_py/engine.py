# SoundForge G6 — ctypes loader + bindings for the host shared library.
# (PLAN_G6.md §3.2 D2, §3.3 D3, P1 phase.)
"""Stdlib-only ctypes binding to libsoundforge.so (host-side Python layer).

Lazy-load access to the C ABI via ``ctypes.CDLL``. Importing this module never
fails without a build; calling a binding without a ``.so`` raises
``RuntimeError`` naming both candidate paths and a ``cmake --build`` hint.

Path contract (D3):
  1. ``$SOUNDFORGE_LIB_PATH`` (env): must be **absolute**, ``realpath``
     -canonicalized, ``isfile``-checked; non-absolute values rejected with
     ``RuntimeError``.
  2. ``REPO_ROOT/native/build/libsoundforge.so`` — default (reg build).
  3. ``RuntimeError`` naming both tried paths + ``cmake --build native/build``.

Security posture (SEC-G6-01/02/08):
  - Loader is **test-only**. It reads the env var **once** at singleton
    creation; setting ``SOUNDFORGE_LIB_PATH`` is a trust boundary equivalent to
    writing into the build tree.
  - The CDLL is opened with explicit ``RTLD_LOCAL`` (never ``RTLD_GLOBAL``); the
    ``.so``'s internal mangled C++ symbols can neither interpose on nor be
    interposed by other DSOs in the Python process.

Wrapped surface (P1): version (3), schema (1), migration (1), project (17),
string/error utils (3), command queue (2). The audio engine (11 exports) is P2
— see the clearly marked stub section at the bottom.

Memory rules (SEC-G6-03/04/06):
  - ``_as_bytes`` derives the C length from the encoded bytes (byte length,
    never char count) for **all** JSON entry calls.
  - ``_take_string`` is the **only** path that frees native memory: copy with
    ``string_at``, zero the pointer, ``sf_free_string`` exactly once. Raw
    ``char*`` never escapes this module.
  - No ``__del__``/finalizers anywhere — destruction is explicit via context
    managers (``Project``, ``CommandQueue``). Create order **project → queue →
    engine**; destroy in reverse (``AudioEngine`` arrives in P2 and will hold
    strong references to its queue and project).

Errors: ``sf_error_string`` is header-inline and **not** exported. After a
failing call read ``last_error(handle)`` / ``last_error_global()`` same-thread,
immediately — the text is valid only until the next error-set (SEC-G6-04).
"""

from __future__ import annotations

import ctypes
import os
from ctypes import (
    CDLL,
    RTLD_LOCAL,
    POINTER,
    c_char_p,
    c_double,
    c_int32,
    c_size_t,
    c_void_p,
    cast,
    create_string_buffer,
    string_at,
)
from pathlib import Path

__all__ = [
    "SfError",
    "get_lib",
    "engine_version",
    "schema_version",
    "is_compatible",
    "validate_project_json",
    "migrate_json",
    "last_error",
    "last_error_global",
    "Project",
    "CommandQueue",
]

# REPO_ROOT resolved file-relatively: python/soundforge_py/engine.py -> repo.
REPO_ROOT = Path(__file__).resolve().parents[2]

SF_OK = 0


class SfError(RuntimeError):
    """A native ``sf_*`` call returned a non-``SF_OK`` result.

    Carries the numeric result ``code`` and the error text read immediately
    after the failing call (handle error when a handle was in play, else the
    thread-local error).
    """

    def __init__(self, symbol: str, code: int, message: str) -> None:
        self.symbol = symbol
        self.code = code
        self.message = message
        detail = f" ({message})" if message else ""
        super().__init__(f"{symbol} failed: SF result {code}{detail}")


# --- Lazy singleton loader ------------------------------------------------

_lib: CDLL | None = None


def _candidate_paths() -> dict[str, str]:
    return {
        "env": os.environ.get("SOUNDFORGE_LIB_PATH", ""),
        "default": str(REPO_ROOT / "native" / "build" / "libsoundforge.so"),
    }


def _find_lib_path() -> str:
    """Resolve ``libsoundforge.so`` per the D3 contract (or raise RuntimeError)."""
    cand = _candidate_paths()
    lines = [
        f"  - $SOUNDFORGE_LIB_PATH = {cand['env']!r}"
        + ("" if cand["env"] else " (unset)"),
        f"  - {cand['default']}",
    ]
    hint = "Build with: cmake --build native/build"

    if cand["env"]:
        env_path = cand["env"]
        if not os.path.isabs(env_path):
            raise RuntimeError(
                "SOUNDFORGE_LIB_PATH must be an absolute path "
                f"(got {env_path!r}).\nCandidates:\n" + "\n".join(lines) + f"\n{hint}"
            )
        real = os.path.realpath(env_path)
        if not os.path.isfile(real):
            raise RuntimeError(
                f"libsoundforge.so not found at SOUNDFORGE_LIB_PATH "
                f"(resolved {real!r}).\nCandidates:\n" + "\n".join(lines) + f"\n{hint}"
            )
        return real

    if os.path.isfile(cand["default"]):
        return cand["default"]

    raise RuntimeError(
        "libsoundforge.so not found.\nCandidates:\n" + "\n".join(lines) + f"\n{hint}"
    )


# (symbol, argtypes, restype) — explicit signatures for EVERY wrapped symbol.
_SIGNATURES = [
    # Version (sf_version.h).
    ("sf_engine_version", [], c_char_p),
    ("sf_schema_version", [], c_int32),
    ("sf_is_compatible", [c_int32], c_int32),
    # Schema (sf_schema.h).
    ("sf_validate_project_json", [c_char_p, c_size_t, c_char_p, c_size_t], c_int32),
    # Migration (sf_migration.h).
    ("sf_migrate_json", [c_void_p, POINTER(c_size_t), c_size_t, c_int32, c_int32], c_int32),
    # Project lifecycle (sf_project.h).
    ("sf_project_create", [c_char_p, c_char_p], c_void_p),
    ("sf_project_destroy", [c_void_p], None),
    ("sf_project_clone", [c_void_p, POINTER(c_void_p)], c_int32),
    ("sf_project_to_json", [c_void_p, POINTER(c_char_p), POINTER(c_size_t)], c_int32),
    ("sf_project_from_json", [c_char_p, c_size_t, POINTER(c_void_p)], c_int32),
    ("sf_project_save_to_path", [c_void_p, c_char_p], c_int32),
    ("sf_project_open_from_path", [c_char_p, POINTER(c_void_p)], c_int32),
    ("sf_project_get_name", [c_void_p], c_char_p),
    ("sf_project_get_id", [c_void_p], c_char_p),
    ("sf_project_get_schema_version", [c_void_p], c_int32),
    ("sf_project_get_engine_version", [c_void_p], c_char_p),
    ("sf_project_rename", [c_void_p, c_char_p], c_int32),
    ("sf_venue_rename", [c_void_p, c_char_p], c_int32),
    ("sf_venue_set_dimensions", [c_void_p, c_double, c_double, c_double], c_int32),
    ("sf_scene_set_geometry", [c_void_p] + [c_double] * 6, c_int32),
    ("sf_scene_rename", [c_void_p, c_char_p], c_int32),
    ("sf_project_health_check", [c_void_p, c_char_p, c_size_t], c_int32),
    # String / error utils.
    ("sf_free_string", [c_void_p], None),
    ("sf_last_error", [c_void_p], c_char_p),
    ("sf_last_error_global", [], c_char_p),
    # Command queue (create/destroy only; engine binds a caller-owned queue).
    ("sf_cmd_queue_create", [POINTER(c_void_p)], c_int32),
    ("sf_cmd_queue_destroy", [c_void_p], None),
]


def get_lib() -> CDLL:
    """Lazy-load ``libsoundforge.so`` (cached; env read once at first call)."""
    global _lib
    if _lib is None:
        lib = CDLL(_find_lib_path(), mode=RTLD_LOCAL)
        for name, argtypes, restype in _SIGNATURES:
            fn = getattr(lib, name)
            fn.argtypes = argtypes
            fn.restype = restype
        _lib = lib
    return _lib


def _binding(sym: str):
    """Return the wrapped symbol with its explicit signature applied."""
    return getattr(get_lib(), sym)


# --- Conversion helpers ---------------------------------------------------


def _as_bytes(data: str | bytes) -> tuple[ctypes.Array, c_size_t]:
    """Encode JSON input for the native ``char*`` + ``size_t`` entry points.

    ``create_string_buffer(s.encode("utf-8"))`` (NUL-terminated) plus the
    **byte** length ``len(s.encode("utf-8"))`` — never the character count: a
    char-count length on non-ASCII input is an OOB read in the .so
    (SEC-G6-03). Used by **all** JSON entry calls (``sf_validate_project_json``,
    ``sf_project_from_json``, ``sf_migrate_json``).
    """
    raw = data.encode("utf-8") if isinstance(data, str) else bytes(data)
    return create_string_buffer(raw), c_size_t(len(raw))


def _take_string(out: c_char_p) -> str:
    """Copy + free a native string-out exactly once (SEC-G6-04, ORC-G6-14).

    ``out`` is the ``c_char_p`` that received the native pointer. The pointer is
    copied with ``string_at``, zeroed, then released with ``sf_free_string``
    invoked exactly once via ``cast(out, c_void_p)`` (``out.contents`` raises
    ``AttributeError`` on a ``c_char_p``). Raw ``char*`` never escapes.
    """
    addr = cast(out, c_void_p).value
    if not addr:
        return ""
    try:
        text = string_at(addr).decode("utf-8")
    finally:
        out.value = None
        _binding("sf_free_string")(c_void_p(addr))
    return text


def _msg(handle: int | None) -> str:
    return last_error(handle) if handle else last_error_global()


# --- Version surface ------------------------------------------------------


def engine_version() -> str:
    """Compiled-in engine version, e.g. ``"0.1.0-g5"`` (lockstep vs ``__version__``)."""
    raw = _binding("sf_engine_version")()
    return raw.decode("utf-8") if raw else ""


def schema_version() -> int:
    """Canonical schema version (== ``SCHEMA_VERSION``)."""
    return int(_binding("sf_schema_version")())


def is_compatible(schema_version_value: int) -> bool:
    """True if a document with this schemaVersion can be opened (natively or via migration)."""
    return bool(_binding("sf_is_compatible")(c_int32(schema_version_value)))


# --- Schema ---------------------------------------------------------------


def validate_project_json(data: str | bytes) -> tuple[int, str]:
    """Structurally validate project JSON.

    Returns ``(result_code, error_message)``; ``result_code == SF_OK`` (0) with
    an empty message means valid. The **raw bytes** are fed to the native side
    (byte-identity rule, SEC-G6-07) — callers pass file bytes unchanged.
    """
    raw, length = _as_bytes(data)
    err_buf = create_string_buffer(4096)
    code = int(
        _binding("sf_validate_project_json")(
            raw, length, err_buf, c_size_t(4096)
        )
    )
    message = err_buf.value.decode("utf-8") if err_buf.value else ""
    return code, message


# --- Migration ------------------------------------------------------------


def migrate_json(data: str | bytes, from_ver: int, to_ver: int) -> tuple[int, str]:
    """Migrate serialized project JSON in place (native ``sf_migrate_json``).

    Returns ``(result_code, migrated_json_str)``; on failure the string is
    empty. Buffer sized ``max(8*len(raw), 1<<16)`` (ORC-G6-08); ``*inout_len``
    excludes the NUL and is capped at the buffer size.
    """
    raw, length = _as_bytes(data)
    n = int(length.value)
    cap = max(8 * n, 1 << 16)
    buf = create_string_buffer(raw.raw[:n] + b"\0", size=cap)
    inout_len = c_size_t(n)
    code = int(
        _binding("sf_migrate_json")(
            cast(buf, c_void_p),
            ctypes.byref(inout_len),
            c_size_t(cap),
            c_int32(from_ver),
            c_int32(to_ver),
        )
    )
    if code != SF_OK:
        return code, ""
    return code, bytes(buf[: inout_len.value]).decode("utf-8")


# --- Error reporting ------------------------------------------------------


def last_error(handle: int | None = None) -> str:
    """Handle error text when ``handle`` is set, else the thread-local error.

    Valid only until the next error-set — read same-thread immediately after a
    failing call (SEC-G6-04). Never frees (native/internal storage).
    """
    raw = _binding("sf_last_error")(c_void_p(handle) if handle else c_void_p(None))
    return raw.decode("utf-8") if raw else ""


def last_error_global() -> str:
    """Thread-local error text (same as ``last_error(None)``)."""
    raw = _binding("sf_last_error_global")()
    return raw.decode("utf-8") if raw else ""


# --- Project wrapper ------------------------------------------------------


class Project:
    """Owning wrapper over ``sf_project_t`` (no finalizer — explicit close).

    Usage::

        with Project.create("Demo") as p:
            print(p.name, p.engine_version)

    Create order is project → queue → engine; destroy in reverse (P2). Methods
    raise :class:`SfError` on a non-OK native result.
    """

    def __init__(self, handle: int) -> None:
        self._h = int(handle)

    # -- construction ------------------------------------------------------
    @classmethod
    def create(cls, name: str, author: str | None = None) -> "Project":
        name_b = name.encode("utf-8")
        author_b = author.encode("utf-8") if author is not None else None
        handle = _binding("sf_project_create")(name_b, author_b)
        if not handle:
            raise SfError("sf_project_create", -1, last_error_global())
        return cls(handle)

    @classmethod
    def from_json(cls, data: str | bytes) -> "Project":
        raw, length = _as_bytes(data)
        out = c_void_p()
        code = int(
            _binding("sf_project_from_json")(raw, length, ctypes.byref(out))
        )
        if code != SF_OK:
            raise SfError("sf_project_from_json", code, last_error_global())
        return cls(out.value or 0)

    @classmethod
    def open_from_path(cls, path: str) -> "Project":
        out = c_void_p()
        code = int(
            _binding("sf_project_open_from_path")(
                path.encode("utf-8"), ctypes.byref(out)
            )
        )
        if code != SF_OK:
            raise SfError("sf_project_open_from_path", code, last_error_global())
        return cls(out.value or 0)

    # -- lifecycle ---------------------------------------------------------
    def clone(self) -> "Project":
        out = c_void_p()
        code = int(
            _binding("sf_project_clone")(c_void_p(self._h), ctypes.byref(out))
        )
        if code != SF_OK:
            raise SfError("sf_project_clone", code, self._err())
        return Project(out.value or 0)

    def close(self) -> None:
        """Destroy the handle (idempotent). Must run before queue/engine destroy."""
        if self._h:
            _binding("sf_project_destroy")(c_void_p(self._h))
            self._h = 0

    def __enter__(self) -> "Project":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- codec -------------------------------------------------------------
    def to_json(self) -> str:
        out_json = c_char_p()
        out_len = c_size_t()
        code = int(
            _binding("sf_project_to_json")(
                c_void_p(self._h), ctypes.byref(out_json), ctypes.byref(out_len)
            )
        )
        if code != SF_OK:
            raise SfError("sf_project_to_json", code, self._err())
        return _take_string(out_json)

    def save_to_path(self, path: str) -> None:
        code = int(
            _binding("sf_project_save_to_path")(
                c_void_p(self._h), path.encode("utf-8")
            )
        )
        if code != SF_OK:
            raise SfError("sf_project_save_to_path", code, self._err())

    # -- accessors (native static/internal storage — never freed) ----------
    def _str_getter(self, sym: str) -> str:
        raw = _binding(sym)(c_void_p(self._h))
        return raw.decode("utf-8") if raw else ""

    @property
    def name(self) -> str:
        return self._str_getter("sf_project_get_name")

    @property
    def id(self) -> str:
        return self._str_getter("sf_project_get_id")

    @property
    def schema_version(self) -> int:
        return int(_binding("sf_project_get_schema_version")(c_void_p(self._h)))

    @property
    def engine_version(self) -> str:
        """The fixture's **own** ``engineVersion`` (e.g. ``0.1.0-g1``), never ``__version__`` (ORC-G6-09)."""
        return self._str_getter("sf_project_get_engine_version")

    # -- mutators ----------------------------------------------------------
    def _mutate(self, sym: str, *args: object) -> None:
        code = int(_binding(sym)(c_void_p(self._h), *args))
        if code != SF_OK:
            raise SfError(sym, code, self._err())

    def rename(self, new_name: str) -> None:
        self._mutate("sf_project_rename", new_name.encode("utf-8"))

    def venue_rename(self, new_name: str) -> None:
        self._mutate("sf_venue_rename", new_name.encode("utf-8"))

    def venue_set_dimensions(self, width_m: float, depth_m: float, height_m: float) -> None:
        self._mutate(
            "sf_venue_set_dimensions",
            c_double(width_m),
            c_double(depth_m),
            c_double(height_m),
        )

    def scene_set_geometry(
        self, cx: float, cy: float, cz: float, lx: float, ly: float, lz: float
    ) -> None:
        self._mutate(
            "sf_scene_set_geometry",
            *[c_double(v) for v in (cx, cy, cz, lx, ly, lz)],
        )

    def scene_rename(self, new_name: str) -> None:
        self._mutate("sf_scene_rename", new_name.encode("utf-8"))

    def health_check(self, cap: int = 8192) -> tuple[int, str]:
        """Return ``(code, report_json)``; ``code==SF_OK`` means status ``"ok"``."""
        buf = create_string_buffer(cap)
        code = int(
            _binding("sf_project_health_check")(c_void_p(self._h), buf, c_size_t(cap))
        )
        return code, (buf.value.decode("utf-8") if buf.value else "")

    def _err(self) -> str:
        return last_error(self._h)


# --- Command queue wrapper ------------------------------------------------


class CommandQueue:
    """Owning wrapper over ``sf_cmd_queue_t`` (no finalizer — explicit close).

    A queue must outlive any engine that binds it; destroy the engine first,
    then the queue, then the project (SEC-G6-06).
    """

    def __init__(self, handle: int) -> None:
        self._h = int(handle)

    @classmethod
    def create(cls) -> "CommandQueue":
        out = c_void_p()
        code = int(_binding("sf_cmd_queue_create")(ctypes.byref(out)))
        if code != SF_OK:
            raise SfError("sf_cmd_queue_create", code, last_error_global())
        return cls(out.value or 0)

    def close(self) -> None:
        if self._h:
            _binding("sf_cmd_queue_destroy")(c_void_p(self._h))
            self._h = 0

    def __enter__(self) -> "CommandQueue":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


# --- Audio engine (P2 — intentionally NOT bound yet) ----------------------
# PLAN_G6.md §3.3 wraps the 11 sf_audio_engine_* exports in P2, together with
# the AudioEngine context manager, CFUNCTYPE read/write io callbacks, and the
# dc/sine signal-source + block-capture helpers. Do not add them in P1.
