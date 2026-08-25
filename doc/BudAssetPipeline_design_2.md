The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.

    + Geometry Flags

    + Traditional Mesh Offset

    + Virtual Geometry Offset

    + Ray Tracing Offset

    + Collision Offset


Example flags:

HasTraditionalMesh

HasVirtualGeometry

HasRayTracingData

HasCollisionData


Runtime chooses the required representation according to rendering needs.


## 31. Virtual Geometry Metadata

Virtual Geometry metadata describes VG data layout.


Example:

Virtual Geometry Metadata

    + Cluster Count

    + Page Count

    + Cluster Buffer Offset

    + Page Table Offset

    + Streaming Information


Runtime uses this information for:

- DAG traversal
- Page streaming
- GPU upload


## 32. Traditional Mesh Metadata

Traditional mesh metadata describes normal mesh representation.


Example:

Traditional Mesh Metadata

    + Vertex Buffer Offset

    + Index Buffer Offset

    + LOD Count

    + Section Count


Section information:

Mesh Section

    + Index Offset

    + Index Count

    + Material Slot

    + Flags


This representation supports dynamic objects and legacy rendering.


## 33. Asset Bulk Data

Large data is stored separately from metadata.

Example:

Building.budasset

    Header
    Metadata
    References


Bulk Data

    + Geometry Data
    + Texture Data
    + Animation Data


Benefits:

- Streaming support
- Memory mapping
- Large asset support
- Faster loading


## 34. Dependency System

Assets reference each other through AssetID.


Example:

Character.budasset

    |
    +-- Mesh AssetID

    +-- Material AssetID

    +-- Skeleton AssetID

    +-- Animation AssetID


No raw pointers are stored.


Advantages:

- Serialization safety
- Asset relocation support
- Streaming support
- Dependency analysis


## 35. Asset Registry

AssetRegistry maintains runtime and editor asset information.

Responsibilities: