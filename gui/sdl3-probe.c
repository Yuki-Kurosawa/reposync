#include <SDL3/SDL.h>
#include <stdio.h>

int main(void)
{
    printf("SDL3 deep probe\n");
    if (SDL_Init(0) != 0) {
        printf("INIT(0) FAIL: [%s]\n", SDL_GetError());
    } else {
        printf("INIT(0) ok\n");
    }
    int n = SDL_GetNumVideoDrivers();
    printf("video drivers: %d\n", n);
    for (int i = 0; i < n; i++)
        printf("  driver[%d] = %s\n", i, SDL_GetVideoDriver(i));
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("INIT(VIDEO) FAIL: [%s]\n", SDL_GetError());
        return 1;
    }
    printf("INIT(VIDEO) ok, current=%s\n", SDL_GetCurrentVideoDriver());
    SDL_Quit();
    return 0;
}
