# xv6-allocator-slab
Modification to the xv6 operating system, replaced their freelist allocator with a multilayered allocator, relying on a buddy allocator for contiguous pages and page management, and a slab alloactor for caching objects and managing small fixed size buffers.

After implementing the allocator, most of the kernel objects that used to be statically allocated are now dynamically allocated using the slab caches.
