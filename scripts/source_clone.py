"""Clone source without volatile Python bytecode, pytest cache and ccache names.

Parallel guards and pytest write and remove __pycache__, *.pyc, .pytest_cache,
and pytest-cache-files-* while a clone is listing the tree. The compile steps of
the CI jobs keep their ccache in the repository's own .ccache (CCACHE_DIR) and
write and remove temporary files there while a guard clones the tree.
"""
import shutil


VOLATILE_PATTERNS = ("__pycache__", "*.pyc", ".pytest_cache", "pytest-cache-files-*", ".ccache")


def clone_source_tree(source, destination, extra_ignore=(), copy_function=shutil.copy2):
    return shutil.copytree(source, destination, ignore=shutil.ignore_patterns(*extra_ignore, *VOLATILE_PATTERNS), copy_function=copy_function)
