The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.
# Bud Engine Material System

Version: 1.0

---

# 1. Overview

Material System defines how surface appearance data is stored, cooked, loaded and consumed by the renderer.

The system separates:

- Material Definition
- Shader Logic
- Material Parameters
- Texture References
- GPU Runtime Data

Core principle:

    Material is data, not rendering state.

The renderer consumes material data but does not own material lifetime.

---

# 2. Material Architecture

High level structure:

    Material Asset

        |

        + Material Definition

        + Shader Reference

        + Parameter Layout

        + Texture References

        + Runtime Metadata


Runtime:


    Material AssetID

        |

        v

    Material Handle

        |

        v

    GPU Material Entry

        |

        v

    Shader Binding

---

# 3. Material Asset

Material Asset stores persistent material information.

Example:

    material.budasset

Contains:

    Material Metadata

    Shader Information

    Parameter Description

    Texture AssetID References


Material Asset does not store:

- Runtime GPU resources
- Descriptor handles
- Temporary state

---

# 4. Material Definition

Material Definition describes the material behavior.

Example:

    Material Definition

        Base Color

        Metallic

        Roughness

        Normal Map

        Emission

        Custom Parameters


The definition is renderer independent.

---

# 5. Shader Reference

Material references shaders through identifiers.

Example:

    Material

        |

        v

    Shader AssetID


Runtime resolves:

    Shader AssetID

        |

        v

    Shader Handle


No direct pointer reference exists.

---

# 6. Material Parameters

Material parameters are stored as structured data.

Example:

    struct MaterialParameters
    {
        float4 baseColor;

        float metallic;

        float roughness;

        float emission;
    };


Parameters are separated from shader code.

---

# 7. Parameter Layout

Shader parameters require a known memory layout.

Example:

    Material Layout

        Offset 0

            BaseColor


        Offset 16

            Metallic


        Offset 20

            Roughness


The layout is generated during cooking.

---

# 8. Material Instance

Material Instance provides parameter variation.

Example:

    Base Material

        |

        + Character Instance

        + Vehicle Instance

        + Building Instance


Instances override parameters without duplicating shader data.

---

# 9. Material Instance Data

Example:

    struct MaterialInstance
    {
        MaterialHandle parent;

        ParameterBuffer parameters;

        TextureOverrides textures;
    };


The instance references the base material.

---

# 10. Texture References

Material references textures through AssetID.


Example:

    Material

