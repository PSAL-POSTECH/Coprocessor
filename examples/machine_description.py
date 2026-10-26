# The machine description (YAML) as a gem5 config script reads it: the keys and values a unit hands its model.
import os
import sys

import yaml

YAML_NULL = "tag:yaml.org,2002:null"


# The machine description as vcix_config in include/coprocessor.h defines it.
def machine_description(path):
    with open(path) as f:
        root = next(yaml.compose_all(f, Loader=yaml.SafeLoader), None)
    if root is None:
        return {}
    if not isinstance(root, yaml.MappingNode):
        script = os.path.basename(sys.argv[0])
        print(f"{script}: {path}: the top level of a machine description is a mapping", file=sys.stderr)
        sys.exit(1)
    return {
        key.value: value.value
        for key, value in root.value
        if isinstance(key, yaml.ScalarNode) and isinstance(value, yaml.ScalarNode) and value.tag != YAML_NULL
    }
