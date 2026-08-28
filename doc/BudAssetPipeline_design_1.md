The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.


Virtual Geometry uses a hierarchical cluster structure.


Example:


    Root Cluster


        |

        + Cluster Group


                |

                + Cluster

                |

                + Cluster



The hierarchy allows:


- Coarse to fine traversal
- Distance based refinement
- Efficient visibility testing


---

# 5. Cluster Data Structure


Example:


    struct VGCluster
    {
        uint32 id;

        uint32 pageID;

        uint32 childOffset;

        uint32 childCount;

        Bounds bounds;

        float error;
    };


A cluster contains:

- Identity
- Parent/child relationship
- Bounding information
- Error information
- Page reference


---

# 6. Cluster Error Metric


Each cluster stores geometric error.


The renderer uses error to decide:


    Keep Current Cluster


or


    Refine To Child Clusters



Concept:


    Screen Error


        =


    Geometric Error

        /

    Distance



The smaller the object appears on screen, the less detail is required.


---

# 7. Cluster Refinement


Traversal example:


    Root Cluster


        |

        v


    Check Screen Error


        |

        +----------------+

        |                |

        v                v


    Accept Cluster     Refine


                        |

                        v


                   Child Clusters



---
