#define POSIXLY_CORRECT
#include <getopt.h>
#include "../src/vgmstream.h"
#include "../src/util.h"
#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#ifndef VERSION
#include "../version.h"
#endif
#ifndef VERSION
#define VERSION "(unknown version)"
#endif

#define BUFFER_SAMPLES 0x8000

/* getopt globals (the horror...) */
extern char * optarg;
extern int optind, opterr, optopt;


static size_t make_wav_header(uint8_t * buf, size_t buf_size, int32_t sample_count, int32_t sample_rate, int channels, int smpl_chunk, int32_t loop_start, int32_t loop_end);

static void usage(const char * name) {
    fprintf(stderr,"vgmstream CLI decoder " VERSION " " __DATE__ "\n"
            "Usage: %s [-o outfile.wav] [options] infile\n"
            "Options:\n"
            "    -o outfile.wav: name of output .wav file, default infile.wav\n"
            "    -l loop count: loop count, default 2.0\n"
            "    -f fade time: fade time in seconds after N loops, default 10.0\n"
            "    -d fade delay: fade delay in seconds, default 0.0\n"
            "    -F: don't fade after N loops and play the rest of the stream\n"
            "    -i: ignore looping information and play the whole stream once\n"
            "    -e: force end-to-end looping\n"
            "    -E: force end-to-end looping even if file has real loop points\n"
            "    -s N: select subsong N, if the format supports multiple subsongs\n"
            "    -m: print metadata only, don't decode\n"
            "    -L: append a smpl chunk and create a looping wav\n"
            "    -2 N: only output the Nth (first is 0) set of stereo channels\n"
            "    -p: output to stdout (for piping into another program)\n"
            "    -P: output to stdout even if stdout is a terminal\n"
            "    -c: loop forever (continuously) to stdout\n"
            "    -x: decode and print adxencd command line to encode as ADX\n"
            "    -g: decode and print oggenc command line to encode as OGG\n"
            "    -b: decode and print batch variable commands\n"
            "    -r: output a second file after resetting (for testing)\n"
            , name);
}


typedef struct {
    char * infilename;
    char * outfilename;
    int ignore_loop;
    int force_loop;
    int really_force_loop;
    int play_sdtout;
    int play_wreckless;
    int play_forever;
    int print_metaonly;
    int print_adxencd;
    int print_oggenc;
    int print_batchvar;
    int test_reset;
    int write_lwav;
    int only_stereo;
    int stream_index;
    double loop_count;
    double fade_time;
    double fade_delay;
    int ignore_fade;

    /* not quite config but eh */
    int lwav_loop_start;
    int lwav_loop_end;
} cli_config;


static int parse_config(cli_config *cfg, int argc, char ** argv) {
    int opt;

    /* non-zero defaults */
    cfg->only_stereo = -1;
    cfg->loop_count = 2.0;
    cfg->fade_time = 10.0;

    /* don't let getopt print errors to stdout automatically */
    opterr = 0;

    /* read config */
    while ((opt = getopt(argc, argv, "o:l:f:d:ipPcmxeLEFrgb2:s:")) != -1) {
        switch (opt) {
            case 'o':
                cfg->outfilename = optarg;
                break;
            case 'l':
                cfg->loop_count = atof(optarg);
                break;
            case 'f':
                cfg->fade_time = atof(optarg);
                break;
            case 'd':
                cfg->fade_delay = atof(optarg);
                break;
            case 'i':
                cfg->ignore_loop = 1;
                break;
            case 'p':
                cfg->play_sdtout = 1;
                break;
            case 'P':
                cfg->play_wreckless = 1;
                cfg->play_sdtout = 1;
                break;
            case 'c':
                cfg->play_forever = 1;
                break;
            case 'm':
                cfg->print_metaonly = 1;
                break;
            case 'x':
                cfg->print_adxencd = 1;
                break;
            case 'g':
                cfg->print_oggenc = 1;
                break;
            case 'b':
                cfg->print_batchvar = 1;
                break;
            case 'e':
                cfg->force_loop = 1;
                break;
            case 'E':
                cfg->really_force_loop = 1;
                break;
            case 'L':
                cfg->write_lwav = 1;
                break;
            case 'r':
                cfg->test_reset = 1;
                break;
            case '2':
                cfg->only_stereo = atoi(optarg);
                break;
            case 'F':
                cfg->ignore_fade = 1;
                break;
            case 's':
                cfg->stream_index = atoi(optarg);
                break;
            case '?':
                fprintf(stderr, "Unknown option -%c found\n", optopt);
                goto fail;
            default:
                usage(argv[0]);
                goto fail;
        }
    }

    /* filename goes last */
    if (optind != argc - 1) {
        usage(argv[0]);
        goto fail;
    }
    cfg->infilename = argv[optind];


    return 1;
fail:
    return 0;
}

static int validate_config(cli_config *cfg) {
    if (cfg->play_sdtout && (!cfg->play_wreckless && isatty(STDOUT_FILENO))) {
        fprintf(stderr,"Are you sure you want to output wave data to the terminal?\nIf so use -P instead of -p.\n");
        goto fail;
    }
    if (cfg->play_forever && !cfg->play_sdtout) {
        fprintf(stderr,"-c must use -p or -P\n");
        goto fail;
    }
    if (cfg->ignore_loop && cfg->force_loop) {
        fprintf(stderr,"-e and -i are incompatible\n");
        goto fail;
    }
    if (cfg->ignore_loop && cfg->really_force_loop) {
        fprintf(stderr,"-E and -i are incompatible\n");
        goto fail;
    }
    if (cfg->force_loop && cfg->really_force_loop) {
        fprintf(stderr,"-E and -e are incompatible\n");
        goto fail;
    }
    if (cfg->play_sdtout && cfg->outfilename) {
        fprintf(stderr,"either -p or -o, make up your mind\n");
        goto fail;
    }

    return 1;
fail:
    return 0;
}

static void print_info(VGMSTREAM * vgmstream, cli_config *cfg) {
    if (!cfg->play_sdtout) {
        if (cfg->print_adxencd) {
            printf("adxencd");
            if (!cfg->print_metaonly)
                printf(" \"%s\"",cfg->outfilename);
            if (vgmstream->loop_flag)
                printf(" -lps%d -lpe%d",vgmstream->loop_start_sample,vgmstream->loop_end_sample);
            printf("\n");
        }
        else if (cfg->print_oggenc) {
            printf("oggenc");
            if (!cfg->print_metaonly)
                printf(" \"%s\"",cfg->outfilename);
            if (vgmstream->loop_flag)
                printf(" -c LOOPSTART=%d -c LOOPLENGTH=%d",vgmstream->loop_start_sample, vgmstream->loop_end_sample-vgmstream->loop_start_sample);
            printf("\n");
        }
        else if (cfg->print_batchvar) {
            if (!cfg->print_metaonly)
                printf("set fname=\"%s\"\n",cfg->outfilename);
            printf("set tsamp=%d\nset chan=%d\n", vgmstream->num_samples, vgmstream->channels);
            if (vgmstream->loop_flag)
                printf("set lstart=%d\nset lend=%d\nset loop=1\n", vgmstream->loop_start_sample, vgmstream->loop_end_sample);
            else
                printf("set loop=0\n");
        }
        else if (cfg->print_metaonly) {
            printf("metadata for %s\n",cfg->infilename);
        }
        else {
            printf("decoding %s\n",cfg->infilename);
        }
    }

    if (!cfg->play_sdtout && !cfg->print_adxencd && !cfg->print_oggenc && !cfg->print_batchvar) {
        char description[1024];
        description[0] = '\0';
        describe_vgmstream(vgmstream,description,1024);
        printf("%s\n",description);
    }
}

static void apply_config(VGMSTREAM * vgmstream, cli_config *cfg) {

    /* honor suggested config, if any (defined order matters)
     * note that ignore_fade and play_forever should take priority */
    if (vgmstream->config_loop_count) {
        cfg->loop_count = vgmstream->config_loop_count;
    }
    if (vgmstream->config_fade_delay) {
        cfg->fade_delay = vgmstream->config_fade_delay;
    }
    if (vgmstream->config_fade_delay) {
        cfg->fade_time = vgmstream->config_fade_time;
    }
    if (vgmstream->config_force_loop) {
        cfg->really_force_loop = 1;
    }
    if (vgmstream->config_ignore_loop) {
        cfg->ignore_loop = 1;
    }
    if (vgmstream->config_ignore_fade) {
        cfg->ignore_fade = 1;
    }

    /* remove non-compatible options */
    if (cfg->play_forever) {
        cfg->ignore_fade = 0;
        cfg->ignore_loop = 0;
    }

    /* change vgmstream's loop stuff (ignore loop goes last) */
    if (cfg->force_loop && !vgmstream->loop_flag) {
        vgmstream_force_loop(vgmstream, 1, 0,vgmstream->num_samples);
    }
    if (cfg->really_force_loop) {
        vgmstream_force_loop(vgmstream, 1, 0,vgmstream->num_samples);
    }
    if (cfg->ignore_loop) {
        vgmstream_force_loop(vgmstream, 0, 0,0);
    }

    /* loop N times, but also play stream end instead of fading out */
    if (cfg->loop_count > 0 && cfg->ignore_fade) {
        vgmstream_set_loop_target(vgmstream, (int)cfg->loop_count);
        cfg->fade_time = 0;
    }

    /* write loops in the wav, but don't actually loop it */
    if (cfg->write_lwav) {
        cfg->lwav_loop_start = vgmstream->loop_start_sample;
        cfg->lwav_loop_end = vgmstream->loop_end_sample;
        vgmstream_force_loop(vgmstream, 0, 0,0);
    }
}

void apply_fade(sample * buf, VGMSTREAM * vgmstream, int to_get, int i, int len_samples, int fade_samples) {
    if (vgmstream->loop_flag && fade_samples > 0) {
        int samples_into_fade = i - (len_samples - fade_samples);
        if (samples_into_fade + to_get > 0) {
            int j, k;
            for (j = 0; j < to_get; j++, samples_into_fade++) {
                if (samples_into_fade > 0) {
                    double fadedness = (double)(fade_samples - samples_into_fade) / fade_samples;
                    for (k = 0; k < vgmstream->channels; k++) {
                        buf[j*vgmstream->channels+k] = (sample)buf[j*vgmstream->channels+k]*fadedness;
                    }
                }
            }
        }
    }
}

int main(int argc, char ** argv) {
    VGMSTREAM * vgmstream = NULL;
    FILE * outfile = NULL;
    char outfilename_temp[PATH_LIMIT];

    sample * buf = NULL;
    int32_t len_samples;
    int32_t fade_samples;
    int i, j;

    cli_config cfg = {0};
    int res;


    /* read args */
    res = parse_config(&cfg, argc, argv);
    if (!res) goto fail;

#ifdef WIN32
    /* make stdout output work with windows */
    if (cfg.play_sdtout) {
        _setmode(fileno(stdout),_O_BINARY);
    }
#endif

    res = validate_config(&cfg);
    if (!res) goto fail;


    /* open streamfile and pass subsong */
    {
        //s = init_vgmstream(infilename);
        STREAMFILE *streamFile = open_stdio_streamfile(cfg.infilename);
        if (!streamFile) {
            fprintf(stderr,"file %s not found\n",cfg.infilename);
            goto fail;
        }

        streamFile->stream_index = cfg.stream_index;
        vgmstream = init_vgmstream_from_STREAMFILE(streamFile);
        close_streamfile(streamFile);

        if (!vgmstream) {
            fprintf(stderr,"failed opening %s\n",cfg.infilename);
            goto fail;
        }
    }


    /* modify the VGMSTREAM if needed */
    apply_config(vgmstream, &cfg);

    if (cfg.play_forever && (!vgmstream->loop_flag || vgmstream->loop_target > 0)) {
        fprintf(stderr,"I could play a nonlooped track forever, but it wouldn't end well.");
        goto fail;
    }


    /* prepare output */
    if (cfg.play_sdtout) {
        outfile = stdout;
    }
    else if (!cfg.print_metaonly) {
        if (!cfg.outfilename) {
            /* note that outfilename_temp must persist outside this block, hence the external array */
            strcpy(outfilename_temp, cfg.infilename);
            strcat(outfilename_temp, ".wav");
            cfg.outfilename = outfilename_temp;
        }

        outfile = fopen(cfg.outfilename,"wb");
        if (!outfile) {
            fprintf(stderr,"failed to open %s for output\n",cfg.outfilename);
            goto fail;
        }
    }


    /* print file info (or batch commands, depending on config) */
    print_info(vgmstream, &cfg);
    if (cfg.print_metaonly) {
        if (!cfg.play_sdtout)
            fclose(outfile);
        close_vgmstream(vgmstream);
        return EXIT_SUCCESS;
    }


    /* get final play config */
    len_samples = get_vgmstream_play_samples(cfg.loop_count,cfg.fade_time,cfg.fade_delay,vgmstream);
    fade_samples = (int32_t)(cfg.fade_time < 0 ? 0 : cfg.fade_time * vgmstream->sample_rate);

    if (!cfg.play_sdtout && !cfg.print_adxencd && !cfg.print_oggenc && !cfg.print_batchvar) {
        printf("samples to play: %d (%.4lf seconds)\n", len_samples, (double)len_samples / vgmstream->sample_rate);
    }


    /* last init */
    buf = malloc(BUFFER_SAMPLES*sizeof(sample)*vgmstream->channels);
    if (!buf) {
        fprintf(stderr,"failed allocating output buffer\n");
        goto fail;;
    }

    /* slap on a .wav header */
    {
        uint8_t wav_buf[0x100];
        int channels = (cfg.only_stereo != -1) ? 2 : vgmstream->channels;
        size_t bytes_done;

        bytes_done = make_wav_header(wav_buf,0x100,
                len_samples, vgmstream->sample_rate, channels,
                cfg.write_lwav, cfg.lwav_loop_start, cfg.lwav_loop_end);

        fwrite(wav_buf,sizeof(uint8_t),bytes_done,outfile);
    }


    /* decode forever */
    while (cfg.play_forever) {
        int to_get = BUFFER_SAMPLES;

        render_vgmstream(buf,to_get,vgmstream);

        swap_samples_le(buf,vgmstream->channels*to_get); /* write PC endian */
        if (cfg.only_stereo != -1) {
            for (j = 0; j < to_get; j++) {
                fwrite(buf+j*vgmstream->channels+(cfg.only_stereo*2),sizeof(sample),2,outfile);
            }
        } else {
            fwrite(buf,sizeof(sample)*vgmstream->channels,to_get,outfile);
        }
    }


    /* decode */
    for (i = 0; i < len_samples; i += BUFFER_SAMPLES) {
        int to_get = BUFFER_SAMPLES;
        if (i + BUFFER_SAMPLES > len_samples)
            to_get = len_samples - i;

        render_vgmstream(buf,to_get,vgmstream);

        apply_fade(buf, vgmstream, to_get, i, len_samples, fade_samples);

        swap_samples_le(buf,vgmstream->channels*to_get); /* write PC endian */
        if (cfg.only_stereo != -1) {
            for (j = 0; j < to_get; j++) {
                fwrite(buf+j*vgmstream->channels+(cfg.only_stereo*2),sizeof(sample),2,outfile);
            }
        } else {
            fwrite(buf,sizeof(sample)*vgmstream->channels,to_get,outfile);
        }
    }

    fclose(outfile);
    outfile = NULL;


    /* try again with (for testing reset_vgmstream, simulates a seek to 0) */
    if (cfg.test_reset) {
        char outfilename_temp[PATH_LIMIT];
        strcpy(outfilename_temp, cfg.outfilename);
        strcat(outfilename_temp, ".reset.wav");

        outfile = fopen(outfilename_temp,"wb");
        if (!outfile) {
            fprintf(stderr,"failed to open %s for output\n",outfilename_temp);
            goto fail;
        }

        reset_vgmstream(vgmstream);

        /* vgmstream manipulations are undone by reset */
        apply_config(vgmstream, &cfg);


        /* slap on a .wav header */
        {
            uint8_t wav_buf[0x100];
            int channels = (cfg.only_stereo != -1) ? 2 : vgmstream->channels;
            size_t bytes_done;

            bytes_done = make_wav_header(wav_buf,0x100,
                    len_samples, vgmstream->sample_rate, channels,
                    cfg.write_lwav, cfg.lwav_loop_start, cfg.lwav_loop_end);

            fwrite(wav_buf,sizeof(uint8_t),bytes_done,outfile);
        }

        /* decode */
        for (i = 0; i < len_samples; i += BUFFER_SAMPLES) {
            int to_get = BUFFER_SAMPLES;
            if (i + BUFFER_SAMPLES > len_samples)
                to_get = len_samples - i;

            render_vgmstream(buf,to_get,vgmstream);

            apply_fade(buf, vgmstream, to_get, i, len_samples, fade_samples);

            swap_samples_le(buf,vgmstream->channels*to_get); /* write PC endian */
            if (cfg.only_stereo != -1) {
                for (j = 0; j < to_get; j++) {
                    fwrite(buf+j*vgmstream->channels+(cfg.only_stereo*2),sizeof(sample),2,outfile);
                }
            } else {
                fwrite(buf,sizeof(sample)*vgmstream->channels,to_get,outfile);
            }
        }
        fclose(outfile);
        outfile = NULL;
    }

    close_vgmstream(vgmstream);
    free(buf);

    return EXIT_SUCCESS;

fail:
    if (!cfg.play_sdtout)
    {
        if (outfile != NULL)
        {
            fclose(outfile);
        }
    }
    close_vgmstream(vgmstream);
    return EXIT_FAILURE;
}



static void make_smpl_chunk(uint8_t * buf, int32_t loop_start, int32_t loop_end) {
    int i;

    memcpy(buf+0, "smpl", 4);/* header */
    put_32bitLE(buf+4, 0x3c);/* size */

    for (i = 0; i < 7; i++)
        put_32bitLE(buf+8 + i * 4, 0);

    put_32bitLE(buf+36, 1);

    for (i = 0; i < 3; i++)
        put_32bitLE(buf+40 + i * 4, 0);

    put_32bitLE(buf+52, loop_start);
    put_32bitLE(buf+56, loop_end);
    put_32bitLE(buf+60, 0);
    put_32bitLE(buf+64, 0);
}

/* make a RIFF header for .wav */
static size_t make_wav_header(uint8_t * buf, size_t buf_size, int32_t sample_count, int32_t sample_rate, int channels, int smpl_chunk, int32_t loop_start, int32_t loop_end) {
    size_t data_size, header_size;

    data_size = sample_count*channels*sizeof(sample);
    header_size = 0x2c;
    if (smpl_chunk && loop_end)
        header_size += 0x3c+ 0x08;

    if (header_size > buf_size)
        goto fail;

    memcpy(buf+0x00, "RIFF", 4); /* RIFF header */
    put_32bitLE(buf+4, (int32_t)(header_size - 0x08 + data_size)); /* size of RIFF */

    memcpy(buf+0x08, "WAVE", 4); /* WAVE header */

    memcpy(buf+0x0c, "fmt ", 4); /* WAVE fmt chunk */
    put_32bitLE(buf+0x10, 0x10); /* size of WAVE fmt chunk */
    put_16bitLE(buf+0x14, 1); /* compression code 1=PCM */
    put_16bitLE(buf+0x16, channels); /* channel count */
    put_32bitLE(buf+0x18, sample_rate); /* sample rate */
    put_32bitLE(buf+0x1c, sample_rate*channels*sizeof(sample)); /* bytes per second */
    put_16bitLE(buf+0x20, (int16_t)(channels*sizeof(sample))); /* block align */
    put_16bitLE(buf+0x22, sizeof(sample)*8); /* significant bits per sample */

    if (smpl_chunk && loop_end) {
        make_smpl_chunk(buf+0x24, loop_start, loop_end);
        memcpy(buf+0x24+0x3c+0x08, "data", 0x04); /* WAVE data chunk */
        put_32bitLE(buf+0x28+0x3c+0x08, (int32_t)data_size); /* size of WAVE data chunk */
    }
    else {
        memcpy(buf+0x24, "data", 0x04); /* WAVE data chunk */
        put_32bitLE(buf+0x28, (int32_t)data_size); /* size of WAVE data chunk */
    }

    return header_size;
fail:
    return 0;
}
