from __future__ import annotations
import typing
__all__: list[str] = ['IFilter', 'LowPass']
class IFilter:
    pass
class LowPass(IFilter):
    def __init__(self, cutoff_freq: typing.SupportsFloat | typing.SupportsIndex = 10.0) -> None:
        ...
