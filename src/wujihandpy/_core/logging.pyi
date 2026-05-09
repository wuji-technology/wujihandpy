from __future__ import annotations
import typing
__all__: list[str] = ['Level', 'flush', 'set_log_level', 'set_log_path', 'set_log_to_console', 'set_log_to_file']
class Level:
    """
    Members:
    
      Trace
    
      Debug
    
      Info
    
      Warn
    
      Error
    
      Critical
    
      Off
    """
    Critical: typing.ClassVar[Level]  # value = <Level.Critical: 5>
    Debug: typing.ClassVar[Level]  # value = <Level.Debug: 1>
    Error: typing.ClassVar[Level]  # value = <Level.Error: 4>
    Info: typing.ClassVar[Level]  # value = <Level.Info: 2>
    Off: typing.ClassVar[Level]  # value = <Level.Off: 6>
    Trace: typing.ClassVar[Level]  # value = <Level.Trace: 0>
    Warn: typing.ClassVar[Level]  # value = <Level.Warn: 3>
    __members__: typing.ClassVar[dict[str, Level]]  # value = {'Trace': <Level.Trace: 0>, 'Debug': <Level.Debug: 1>, 'Info': <Level.Info: 2>, 'Warn': <Level.Warn: 3>, 'Error': <Level.Error: 4>, 'Critical': <Level.Critical: 5>, 'Off': <Level.Off: 6>}
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
def flush() -> None:
    ...
def set_log_level(value: Level) -> None:
    ...
def set_log_path(value: str) -> None:
    ...
def set_log_to_console(value: bool) -> None:
    ...
def set_log_to_file(value: bool) -> None:
    ...
