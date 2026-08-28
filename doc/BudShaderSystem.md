The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.
# Bud Engine Shader System

Version: 1.0

---

# 1. Overview

Shader System manages:

- Shader source
- Shader compilation
- Shader variants
- Pipeline state generation
- Runtime shader binding

The system separates:

    Shader Definition

        from

    Runtime GPU Shader Object


Core principle:

    Shader is compiled data, not runtime object.

---

# 2. Shader Architecture

High level structure:


    Shader Asset


        |

        + Shader Source


        + Shader Metadata


        + Variant Information


        + Reflection Data



Runtime:


    Shader AssetID


        |

        v


    Shader Handle


        |

        v


    GPU Pipeline Object



---

# 3. Shader Asset

Shader Asset stores shader related information.


Example:


    pbr_shader.budasset


Contains:


    Shader Metadata

    Source Reference

    Entry Points

    Variant Description

    Reflection Information



The asset does not store runtime pipeline state.

---

# 4. Shader Stages

Supported shader stages:


    Vertex Shader


    Pixel Shader


    Compute Shader


    Mesh Shader


    Task Shader


    Ray Tracing Shader



The renderer decides which stages are used.

---

# 5. Shader Source

Source code is stored separately from runtime data.


Example:


    Shader Source


        |

        v


    Shader Compiler


        |

        v


    Compiled Binary



Supported languages:


- HLSL
- GLSL
- Engine Shader Language


---

# 6. Shader Compilation Pipeline


    Shader Source


        |

        v


    Preprocessor


        |

        v


    Compiler


        |

        v


    Reflection


        |

        v


    Backend Compiler


        |

        v


    GPU Binary



---

# 7. Shader Reflection

Reflection extracts runtime information.


Example:


    Shader Reflection


        |

        + Constant Buffers


        + Texture Bindings


        + Resource Types


        + Thread Group Size


        + Vertex Inputs