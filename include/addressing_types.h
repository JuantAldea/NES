#pragma once

enum class Addressing
{
    absolute = 0,
    absolute_X,
    absolute_Y,
    immediate,
    implicit,
    indexed_indirect,
    indirect_indexed,
    addressing_indirect,
    relative,
    zero_page,
    zero_page_X,
    zero_page_Y,
};
