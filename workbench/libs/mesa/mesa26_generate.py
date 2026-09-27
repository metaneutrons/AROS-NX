#!/usr/bin/env python3
"""Mesa 26 generator entry point for the closed compiler/core/aux recipes.

The existing source-immutable generator adapter is version-neutral: it accepts
only an audited script, build-tree output and mode supplied by the transpiler.
Keeping this entry point separate prevents Mesa 26 jobs from silently joining
the Mesa 20 capability while preserving the tested atomic-output behaviour.
"""

from mesa20_generate import main


if __name__ == "__main__":
    main()
