#include "volume_command.h"

#include <cassert>

int main() {
    int volume = 0;
    assert(parse_volume_command("10", 2, &volume));
    assert(volume == 10);
    assert(parse_volume_command(" 1\n", 3, &volume));
    assert(volume == 1);
    assert(!parse_volume_command("0", 1, &volume));
    assert(!parse_volume_command("11", 2, &volume));
    assert(!parse_volume_command("loud", 4, &volume));
}
