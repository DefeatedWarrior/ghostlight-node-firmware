# Load the ESP-IDF v5.2.6 environment for this project.
#   . tools/idf_env.sh        (bash or zsh, from the repo root)
# Then: idf.py build / idf.py -p PORT flash monitor
export IDF_PATH="${IDF_PATH_OVERRIDE:-$HOME/esp/esp-idf}"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf5.2_py3.11_env}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_PATH" >&2
    echo "git clone -b v5.2.6 --depth 1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf" >&2
    return 1 2>/dev/null || exit 1
fi
. "$IDF_PATH/export.sh" > /dev/null
