from typing import Any, Dict, List, Optional, Union

import torch

class GlobalStateGuard:
    def check(self) -> bool: ...
    def reason(self) -> str: ...

class LeafGuard: ...
class GuardDebugInfo: ...

class GuardManager:
    def check(self, value) -> bool: ...
    def check_verbose(self, value) -> GuardDebugInfo: ...

    # Accessors
    def globals_dict_manager(
        self, f_globals: Dict[str, Any], source, example_value
    ) -> GuardManager: ...
    def dict_getitem_manager(self, key, source, example_value) -> GuardManager: ...
    def global_weakref_manager(
        self, global_name: str, source, example_value
    ) -> GuardManager: ...
    def type_manager(self, source, example_value) -> GuardManager: ...
    def getattr_manager(self, attr: str, source, example_value) -> GuardManager: ...
    def lambda_manager(self, python_lambda, source, example_value) -> GuardManager: ...

    # Leaf guards
    def add_lambda_guard(self, user_lambda, verbose_code_parts: List[str]) -> None: ...
    def add_id_match_guard(self, id_val, verbose_code_parts: List[str]) -> None: ...
    def add_equals_match_guard(
        self, equals_val, verbose_code_parts: List[str]
    ) -> None: ...
    def add_global_state_guard(self, verbose_code_parts: List[str]) -> None: ...

class RootGuardManager(GuardManager):
    def get_epilogue_lambda_guards(self) -> List[LeafGuard]: ...
    def add_epilogue_lambda_guard(
        self, guard: LeafGuard, verbose_code_parts: List[str]
    ) -> None: ...

class DictGuardManager(GuardManager):
    def get_key_manager(self, index, source, example_value) -> GuardManager: ...
    def get_value_manager(self, index, source, example_value) -> GuardManager: ...

def install_tensor_aliasing_guard(
    guard_managers: List[GuardManager],
    tensor_names: List[str],
    verbose_code_parts: List[str],
): ...
def install_no_tensor_aliasing_guard(
    guard_managers: List[GuardManager],
    tensor_names: List[str],
    verbose_code_parts: List[str],
): ...

class TensorGuards:
    def __init__(
        self,
        *,
        dynamic_dims_sizes: Optional[List[Optional[torch.SymInt]]] = None,
        dynamic_dims_strides: Optional[List[Optional[torch.SymInt]]] = None,
    ): ...
    def check(self, *args) -> bool: ...
    def check_verbose(self, *args, tensor_check_names=None) -> Union[bool, str]: ...

def assert_size_stride(
    item: torch.Tensor, size: torch.types._size, stride: torch.types._size
): ...
def check_obj_id(obj: object, expected: int) -> bool: ...
def check_type_id(obj: object, expected: int) -> bool: ...
def dict_version(d: Dict[Any, Any]) -> int: ...
