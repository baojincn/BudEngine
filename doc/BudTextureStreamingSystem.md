The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.


Runtime loads only required mips.

---

# 7. Texture Streaming Flow


    Camera


        |

        v


    Visible Objects


        |

        v


    Material Usage


        |

        v


    Texture Importance Analysis


        |

        v


    Mip Request


        |

        v


    Streaming Manager


        |

        v


    GPU Upload



---

# 8. Texture Importance Calculation


Streaming priority can consider:


## Screen Size


Larger screen coverage requires higher mip.


## Distance


Closer objects require higher detail.


## Material Importance


Visible materials receive higher priority.


## Camera Movement


Objects entering view need faster loading.

---

# 9. Texture Residency


Runtime tracks texture state.


Example:


    Texture State


        Unloaded


            |


            v


        Loading


            |


            v


        Resident


            |


            v


        Evicting



---

# 10. GPU Texture Memory Management


GPU texture pool manages memory.


Example:


    Texture Memory Pool


        Slot 0


        Slot 1


        Slot 2



Textures occupy memory according to current mip residency.

---

# 11. Texture Page Storage


Large textures can be divided into pages.


Example:


    Texture Data


        |

        + Page 0


        + Page 1


        + Page 2



Benefits:

- Partial loading
- Better memory control
- Large texture support

---

# 12. Virtual Texture Integration


Optional virtual texture path:


    Texture Space


        |

        v


    Virtual Texture Pages


        |

        v


    Page Cache


        |

        v


    GPU Texture



Similar concept to Virtual Geometry.

---

# 13. Compression


Texture compression is selected during cooking.


Examples:


GPU formats:

- BCn
- ASTC
- ETC

