#ifndef SHATTER_H
#define SHATTER_H

/* Capture the current character frame as falling particles. */
void shatter_spawn(const char *grid, int cols, int rows);
/* Advance the simulation by dt seconds. */
void shatter_step(double dt);
/* Replace grid with the current particle picture. */
void shatter_draw(char *grid, int cols, int rows);
/* Seed the particle random generator (a fixed seed gives reproducible debris). */
void shatter_seed(unsigned int seed);
/* Release all particle memory. */
void shatter_free(void);

#endif /* SHATTER_H */
