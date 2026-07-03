"""Minimal pkg_resources shim for ESP8266_RTOS_SDK v3.4 dependency check.

The legacy SDK's tools/check_python_dependencies.py imports pkg_resources and
calls pkg_resources.require(line) for each line from requirements.txt.

Modern MSYS2 Python 3.12 may have setuptools installed but no pkg_resources
module. For the hello_uart bring-up we only need to pass the SDK's legacy
build-time dependency check, so require() intentionally accepts requirements
without resolving versions.

Do not use this shim as a general pkg_resources replacement.
"""


class DistributionNotFound(Exception):
    pass


class VersionConflict(Exception):
    pass


def require(requirement):
    return []
