/**
 * @file basic.c
 * @brief Minimal C89 receiver example for a materialized Markdown render.
 *
 * Initializes options and releases the result with the same receiver that
 * allocated it before destroying that receiver. For producer-to-consumer
 * streaming, use the source/sink or incremental operations in <libmdf/mdf.h>.
 */
#include <libmdf/mdf.h>

#include <stdio.h>

int main(void)
{
    mdf *renderer;
    mdf_options opts;
    char *out;
    mdf_status st;

    mdf_options_init(&opts);
    opts.boring = 1;
    renderer = NULL;
    out = NULL;
    st = mdf_create(MDF_FORMAT_ANSI, &opts, &renderer);
    if (st != MDF_OK) {
        return 1;
    }
    st = renderer->render_cstr(renderer, "# hello\n\nworld\n", &out);
    if (st != MDF_OK) {
        renderer->destroy(renderer);
        return 1;
    }
    fputs(out, stdout);
    renderer->string_free(renderer, out);
    renderer->destroy(renderer);
    return 0;
}
