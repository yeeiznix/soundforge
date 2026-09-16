# SoundForge G6 — ctypes loader + bindings for the host shared library.
# (PLAN_G6.md §3.2 D2, §3.3 D3, P1 + P2 phases.)
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

Wrapped surface (P1 + P2): version (3), schema (1), migration (1), project
(17), string/error utils (3), command queue (2), audio engine (11). The audio
engine bindings land in P2 together with the :class:`AudioEngine` context
manager. ``sf_error_string`` is header-inline and deliberately **not** bound.

Memory rules (SEC-G6-03/04/06):
  - ``_as_bytes`` derives the C length from the encoded bytes (byte length,
    never char count) for **all** JSON entry calls.
  - ``_take_string`` is the **only** path that frees native memory: copy with
    ``string_at``, zero the pointer, ``sf_free_string`` exactly once. Raw
    ``char*`` never escapes this module.
  - No ``__del__``/finalizers anywhere — destruction is explicit via context
    managers (``Project``, ``CommandQueue``, ``AudioEngine``). Create order
    project → queue → engine; destroy in reverse (engine → queue → project,
    SEC-G6-06). ``AudioEngine`` holds strong references to its queue and
    project.

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
    c_float,
    c_int32,
    c_size_t,
    c_uint32,
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
    "AudioEngine",
    "AudioEngineConfig",
    "AudioEngineIo",
    "ReadIoFn",
    "WriteIoFn",
    "SF_AUDIO_ENGINE_PACE",
]

# REPO_ROOT resolved file-relatively: python/soundforge_py/engine.py -> repo.
REPO_ROOT = Path(__file__).resolve().parents[2]

SF_OK = 0
SF_E_IO = 5

# SF_AUDIO_ENGINE_PACE flag (sf_audio_engine.h). P2 tests drive ticks
# deterministically with start(0) — no pacer thread.
SF_AUDIO_ENGINE_PACE = 0x1


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


# --- Audio engine callback + config types (ctypes mirrors of the C ABI) ----

# float* const* — host-only device protocol (never crosses JNI).
_FloatPtr = POINTER(c_float)
_FloatPtrPtr = POINTER(_FloatPtr)

# Host device callbacks (sf_audio_engine.h lines 52-63): read fills the next
# input block, write consumes the rendered output block. Both run on the
# engine's render lane; both return int32 (SF_OK / SF_E_IO).
ReadIoFn = ctypes.CFUNCTYPE(c_int32, c_void_p, _FloatPtrPtr, c_int32, c_int32)
WriteIoFn = ctypes.CFUNCTYPE(c_int32, c_void_p, _FloatPtrPtr, c_int32, c_int32)


class AudioEngineIo(ctypes.Structure):
    """``sf_audio_engine_io_t``: user pointer + read/write callbacks."""
    _fields_ = [
        ("user", c_void_p),
        ("read", ReadIoFn),
        ("write", WriteIoFn),
    ]


class AudioEngineConfig(ctypes.Structure):
    """``sf_audio_engine_config_t``: sample_rate/channels/max_block_frames/io."""
    _fields_ = [
        ("sample_rate", c_int32),
        ("channels", c_int32),
        ("max_block_frames", c_int32),
        ("io", AudioEngineIo),
    ]


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
    # Audio engine (P2; sf_audio_engine.h — all 11 exports).
    ("sf_audio_engine_create", [POINTER(c_void_p), c_void_p, c_void_p], c_int32),
    ("sf_audio_engine_destroy", [c_void_p], c_int32),
    ("sf_audio_engine_configure", [c_void_p, POINTER(AudioEngineConfig)], c_int32),
    ("sf_audio_engine_set_output", [c_void_p, c_char_p], c_int32),
    ("sf_audio_engine_start", [c_void_p, c_uint32], c_int32),
    ("sf_audio_engine_stop", [c_void_p], c_int32),
    ("sf_audio_engine_join", [c_void_p], c_int32),
    ("sf_audio_engine_tick", [c_void_p, c_size_t], c_int32),
    ("sf_audio_engine_last_report", [c_void_p, c_char_p, c_size_t], c_int32),
    ("sf_audio_engine_meter_json", [c_void_p, c_char_p, c_size_t], c_int32),
    ("sf_audio_engine_reset_meters", [c_void_p], c_int32),
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


# --- Audio engine wrapper (P2) --------------------------------------------
#
# Callback contract (SEC-G6-05): io callbacks copy data **within the call**
# (the read fills per-channel views; the write copies the rendered block out
# immediately) and never retain/dereference engine-owned pointers after
# return. Bodies are wrapped in try/except: on exception the callback records
# the error in its per-session error list and returns SF_E_IO (never lets it
# escape — ctypes would print and return 0 == SF_OK, treating a half-filled
# input as valid). ``channels == 2`` and ``frames >= 1`` are asserted at the
# top of every callback. The concrete read/write callbacks (DC/sine sources,
# block capture) live in ``python/regression/engine_parity.py``; the CFUNCTYPE
# instances are kept alive as attributes of this wrapper for the engine
# lifetime (the native side stores raw function pointers; dropping the
# CFUNCTYPE would be a dangling pointer on the next tick).


class AudioEngine:
    """Context-manager wrapper over ``sf_audio_engine_t`` (SEC-G6-06).

    Lifecycle: create → configure → set_output → start(0) → tick* →
    stop → join → destroy (one-shot, like the runner). Holds **strong refs**
    to its queue and project; ``__exit__`` destroys in reverse order
    (engine → queue → project). No ``__del__`` — destruction is explicit.

    Usage::

        with Project.from_json(raw) as p:
            with CommandQueue.create() as q:
                with AudioEngine.create(q, p) as e:
                    e.configure(sample_rate=48000, channels=2,
                                max_block_frames=256,
                                read_cb=src.cb, write_cb=cap.cb)
                    e.set_output("out")
                    e.start(0)          # no pacer — tick is deterministic
                    e.tick(256)
                    e.stop()
                    e.join()
    """

    def __init__(self, handle: int, queue: CommandQueue, project: Project) -> None:
        self._h = int(handle)
        self._q = queue  # strong ref — must outlive engine (SEC-G6-06)
        self._p = project  # strong ref — must outlive engine (SEC-G6-06)
        # CFUNCTYPE instances kept alive for the engine lifetime (SEC-G6-05):
        # the native side stores raw function pointers. NULL function pointers
        # (``cast(c_void_p(), ReadIoFn)`` — ctypes won't store None in a
        # CFUNCTYPE-typed struct field) represent the legal "no io callback"
        # silence path (unit test NullIoCallbacksAreSilentAndValid).
        self._read_cb: ReadIoFn = cast(c_void_p(), ReadIoFn)
        self._write_cb: WriteIoFn = cast(c_void_p(), WriteIoFn)

    # -- construction ------------------------------------------------------
    @classmethod
    def create(cls, queue: CommandQueue, project: Project) -> "AudioEngine":
        """Bind queue + project and allocate the engine (CREATED state)."""
        out = c_void_p()
        code = int(
            _binding("sf_audio_engine_create")(
                ctypes.byref(out), c_void_p(queue._h), c_void_p(project._h)
            )
        )
        if code != SF_OK:
            raise SfError("sf_audio_engine_create", code, last_error_global())
        return cls(out.value or 0, queue, project)

    # -- configuration (CREATED state only) --------------------------------
    def configure(
        self,
        sample_rate: int = 48000,
        channels: int = 2,
        max_block_frames: int = 512,
        read_cb: ReadIoFn | None = None,
        write_cb: WriteIoFn | None = None,
    ) -> None:
        """Configure sample_rate/channels/max_block_frames and io callbacks.

        The callbacks (plain Python callables or CFUNCTYPE instances) are
        wrapped in CFUNCTYPE and **retained on this wrapper** so the native
        raw function pointers stay valid for the engine lifetime (SEC-G6-05).
        """
        self._read_cb = (
            ReadIoFn(read_cb) if read_cb is not None else cast(c_void_p(), ReadIoFn)
        )
        self._write_cb = (
            WriteIoFn(write_cb) if write_cb is not None else cast(c_void_p(), WriteIoFn)
        )
        io = AudioEngineIo(
            user=None, read=self._read_cb, write=self._write_cb
        )
        cfg = AudioEngineConfig(
            sample_rate=c_int32(sample_rate),
            channels=c_int32(channels),
            max_block_frames=c_int32(max_block_frames),
            io=io,
        )
        code = int(
            _binding("sf_audio_engine_configure")(
                c_void_p(self._h), ctypes.byref(cfg)
            )
        )
        if code != SF_OK:
            raise SfError("sf_audio_engine_configure", code, self._err())

    def set_output(self, out_node_id: str) -> None:
        """Set the output node id for the render plan (CREATED state only)."""
        code = int(
            _binding("sf_audio_engine_set_output")(
                c_void_p(self._h), out_node_id.encode("utf-8")
            )
        )
        if code != SF_OK:
            raise SfError("sf_audio_engine_set_output", code, self._err())

    # -- lifecycle ---------------------------------------------------------
    def start(self, flags: int = 0) -> None:
        """Start the engine; ``flags=0`` = no pacer (deterministic ticks)."""
        code = int(
            _binding("sf_audio_engine_start")(c_void_p(self._h), c_uint32(flags))
        )
        if code != SF_OK:
            raise SfError("sf_audio_engine_start", code, self._err())

    def stop(self) -> None:
        """Request a stop (RUNNING → STOPPING; idempotent)."""
        code = int(_binding("sf_audio_engine_stop")(c_void_p(self._h)))
        if code != SF_OK:
            raise SfError("sf_audio_engine_stop", code, self._err())

    def join(self) -> None:
        """Reap pacer/runner threads (STOPPED; idempotent)."""
        code = int(_binding("sf_audio_engine_join")(c_void_p(self._h)))
        if code != SF_OK:
            raise SfError("sf_audio_engine_join", code, self._err())

    def tick(self, frames: int) -> None:
        """Deterministic synthetic clock: render ``frames`` (RUNNING only)."""
        code = int(
            _binding("sf_audio_engine_tick")(c_void_p(self._h), c_size_t(frames))
        )
        if code != SF_OK:
            raise SfError("sf_audio_engine_tick", code, self._err())

    # -- meters / reports (safe in any state) ------------------------------
    def meter_json(self, cap: int = 4096) -> str:
        """Per-channel true-peak meter JSON (channels, oversample:4,
        blocksRendered, truePeakLinear[2], truePeakDb[2], clipped[2],
        planValid)."""
        buf = create_string_buffer(cap)
        code = int(
            _binding("sf_audio_engine_meter_json")(
                c_void_p(self._h), buf, c_size_t(cap)
            )
        )
        if code != SF_OK:
            raise SfError("sf_audio_engine_meter_json", code, self._err())
        return buf.value.decode("utf-8") if buf.value else ""

    def reset_meters(self) -> None:
        """Zero the per-channel true-peak latches (FIR tails + blocksRendered
        kept — blocksRendered stays monotonic, not reset)."""
        code = int(_binding("sf_audio_engine_reset_meters")(c_void_p(self._h)))
        if code != SF_OK:
            raise SfError("sf_audio_engine_reset_meters", code, self._err())

    def last_report(self, cap: int = 16384) -> str:
        """Passthrough to the runner's last report (meter_json NEVER embedded)."""
        buf = create_string_buffer(cap)
        code = int(
            _binding("sf_audio_engine_last_report")(
                c_void_p(self._h), buf, c_size_t(cap)
            )
        )
        if code != SF_OK:
            raise SfError("sf_audio_engine_last_report", code, self._err())
        return buf.value.decode("utf-8") if buf.value else ""

    # -- teardown -----------------------------------------------------------
    def close(self) -> None:
        """Destroy the engine handle (idempotent). Must precede queue/project
        destroy (reverse order, SEC-G6-06)."""
        if self._h:
            _binding("sf_audio_engine_destroy")(c_void_p(self._h))
            self._h = 0

    def __enter__(self) -> "AudioEngine":
        return self

    def __exit__(self, *exc: object) -> None:
        # Reverse create order: engine → queue → project (SEC-G6-06).
        # Project.close/CommandQueue.close are idempotent, so wrapping the
        # engine inside outer project/queue context managers is safe too.
        self.close()
        self._q.close()
        self._p.close()

    def _err(self) -> str:
        return last_error(self._h)