#include "app-layer.h"
#include "ll-interface.h"
#include "debug.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static int out_packet_index = 0; // only supports one fd.
static int in_packet_index = 0; // only supports one fd.

void free_control_packet(control_packet packet) {
    for (size_t i = 0; i < packet.n; ++i) {
        free(packet.tlvs[i].value.s);
    }
    free(packet.tlvs);
}

void free_data_packet(data_packet packet) {
    free(packet.data.s);
}

static bool isDATApacket(string packet_str, data_packet* outp) {
    char c = packet_str.s[0];

    if (packet_str.len < 5 || packet_str.s == NULL || c != PCONTROL_DATA) {
        if (TRACE_APP) {
            printf("[APP] isDATApacket() ? 0\n");
        }
        return false;
    }

    int index = (unsigned char)packet_str.s[1];
    unsigned char l2 = packet_str.s[2];
    unsigned char l1 = packet_str.s[3];
    size_t len = (size_t)l1 + 256 * (size_t)l2;

    bool b = len == (packet_str.len - 4);

    if (TRACE_APP) {
        printf("[APP] isDATApacket() ? %d [index=%d len=%lu]\n",(int)b,
            b ? index % 256 : 0, b ? len : 0);
    }

    if (b) {
        string data;

        data.len = len;
        data.s = malloc((len + 1) * sizeof(char));
        memcpy(data.s, packet_str.s + 4, len + 1);

        data_packet out = {index, data};

        *outp = out;

        if (index != in_packet_index % 256) {
            printf("[APP] Error: Expected DATA packet #%d, got #%d\n",
                in_packet_index % 256, index);
        }
    }
    return b;
}

static bool isCONTROLpacket(char c, string packet_str, control_packet* outp) {
    char c_char = packet_str.s[0];
    if (c_char != c) return false;

    if (packet_str.len == 0 || packet_str.s == NULL || c_char != c) return false;

    control_packet out;
    out.c = c;
    out.n = 0;
    
    out.tlvs = malloc(2 * sizeof(tlv));
    size_t reserved = 2;

    size_t j = 1;

    while (j + 2 < packet_str.len) {
        char type = packet_str.s[j++];
        size_t l = (unsigned char)packet_str.s[j++];

        if (j + l > packet_str.len) break;

        if (out.n == reserved) {
            out.tlvs = realloc(out.tlvs, 2 * reserved * sizeof(tlv));
            reserved *= 2;
        }

        string value;

        value.len = l;
        value.s = malloc((l + 1) * sizeof(char));
        memcpy(value.s, packet_str.s + j, l);
        value.s[l] = '\0';

        out.tlvs[out.n++] = (tlv){type, value};

        j += l;
    }

    bool b = j == packet_str.len;

    if (!b) {
        free_control_packet(out);
    } else {
        *outp = out;
    }

    return b;
}

static bool isSTARTpacket(string packet_str, control_packet* outp) {
    bool b = isCONTROLpacket(PCONTROL_START, packet_str, outp);

    if (TRACE_APP) {
        printf("[APP] isSTARTpacket() ? %d\n", (int)b);
    }
    return b;
}

static bool isENDpacket(string packet_str, control_packet* outp) {
    bool b = isCONTROLpacket(PCONTROL_END, packet_str, outp);

    if (TRACE_APP) {
        printf("[APP] isENDpacket() ? %d\n", (int)b);
    }
    return b;
}


bool get_tlv(control_packet control, char type, string* outp) {
    for (size_t i = 0; i < control.n; ++i) {
        if (control.tlvs[i].type == type) {
            string value;
            value.len = control.tlvs[i].value.len;
            value.s = strdup(control.tlvs[i].value.s);
            *outp = value;
            return true;
        }
    }
    return false;
}

bool get_tlv_filename(control_packet control, char** outp) {
    string value;
    bool b = get_tlv(control, PCONTROL_TYPE_FILENAME, &value);
    if (!b) {
        if (TRACE_APP_INTERNALS) {
            printf("[APPCORE] Get TLV filename: FAILED\n");
        }
        return false;
    }

    *outp = value.s;
    
    if (TRACE_APP_INTERNALS) {
        printf("[APPCORE] Get TLV filename: OK [filename=%s]\n", value.s);
    }
    return true;
}

bool get_tlv_filesize(control_packet control, size_t* outp) {
    string value;
    bool b = get_tlv(control, PCONTROL_TYPE_FILESIZE, &value);
    if (!b) {
        if (TRACE_APP_INTERNALS) {
            printf("[APPCORE] Get TLV filesize: FAILED\n");
        }
        return false;
    }

    long parse = strtol(value.s, NULL, 10);
    free(value.s);
    if (parse <= 0) {
        if (TRACE_APP_INTERNALS) {
            printf("[APPCORE] Get TLV filesize: BAD PARSE [long=%ld]\n", parse);
        }
        return false;
    }

    *outp = (size_t)parse;

    if (TRACE_APP_INTERNALS) {
        printf("[APPCORE] Get TLV filesize: OK [filesize=%lu]\n", (size_t)parse);
    }
    return true;
}

static int build_data_packet(string fragment, char index, string* outp) {
    static const size_t mod = 256;
    static const size_t max_len = 0x0ffff;

    if (fragment.len > max_len) return 1;

    string data_packet;

    data_packet.len = fragment.len + 4;
    data_packet.s = malloc((data_packet.len + 1) * sizeof(char));

    data_packet.s[0] = PCONTROL_DATA;
    data_packet.s[1] = index;
    data_packet.s[2] = fragment.len / mod;
    data_packet.s[3] = fragment.len % mod;
    memcpy(data_packet.s + 4, fragment.s, fragment.len + 1);

    if (TRACE_APP_INTERNALS) {
        printf("[APPCORE] Built DP [c=0x%02x index=0x%02x l2=0x%02x l1=0x%02x flen=%lu]\n",
            (unsigned char)data_packet.s[0], (unsigned char)data_packet.s[1],
            (unsigned char)data_packet.s[2], (unsigned char)data_packet.s[3],
            fragment.len);
        if (TEXT_DEBUG) print_stringn(data_packet);
    }

    *outp = data_packet;
    return 0;
}

static int build_tlv_str(char type, string value, string* outp) {
    static const size_t max_len = 0x0ff;

    if (value.len > max_len) return 1;

    string tlv;

    tlv.len = value.len + 2;
    tlv.s = malloc((tlv.len + 3) * sizeof(char));

    tlv.s[0] = type;
    tlv.s[1] = value.len;
    memcpy(tlv.s + 2, value.s, value.len + 1);

    if (TRACE_APP_INTERNALS) {
        printf("[APPCORE] Built TLV [t=0x%02x l=%lu]", type, value.len);
        print_stringn(tlv);
    }

    *outp = tlv;
    return 0;
}

static int build_tlv_uint(char type, long unsigned value, string* outp) {
    char buf[10];
    string tmp;
    tmp.s = buf;
    sprintf(tmp.s, "%lu", value);
    tmp.len = strlen(tmp.s);
    return build_tlv_str(type, tmp, outp);
}

static int build_control_packet(char control, string* tlvp, size_t n, string* outp) {
    string control_packet;
    control_packet.len = 1;

    for (size_t i = 0; i < n; ++i) {
        control_packet.len += tlvp[i].len;
    }

    control_packet.s = malloc((control_packet.len + 1) * sizeof(char));
    char* tmp = control_packet.s + 1;

    control_packet.s[0] = control;
    control_packet.s[1] = '\0';

    for (size_t i = 0; i < n; ++i) {
        memcpy(tmp, tlvp[i].s, tlvp[i].len);
        tmp += tlvp[i].len;
    }

    if (TRACE_APP_INTERNALS) {
        printf("[APPCORE] Built CP [c=0x%02x n=%lu tlen=%lu]\n",
            control, n, control_packet.len);
        if (TEXT_DEBUG) print_stringn(control_packet);
    }

    *outp = control_packet;
    return 0;
}

int send_data_packet(int fd, string packet) {
    int s;

    string data_packet;
    s = build_data_packet(packet, out_packet_index % 256lu, &data_packet);
    if (s != 0) return s;

    if (TRACE_APP) {
        printf("[APP] Sending DATA packet #%d [plen=%lu]\n",
            out_packet_index % 256, packet.len);
    }

    ++out_packet_index;
    s = llwrite(fd, data_packet);
    free(data_packet.s);
    return s;
}

int send_start_packet(int fd, size_t filesize, char* filename) {
    int s;
    string tlvs[2];

    out_packet_index = 0;

    s = build_tlv_uint(PCONTROL_TYPE_FILESIZE,
        filesize, tlvs + FILESIZE_TLV_N);
    if (s != 0) return s;

    s = build_tlv_str(PCONTROL_TYPE_FILENAME,
        string_from(filename), tlvs + FILENAME_TLV_N);
    if (s != 0) return s;

    string start_packet;
    s = build_control_packet(PCONTROL_START, tlvs, 2, &start_packet);
    if (s != 0) return s;

    free(tlvs[0].s);
    free(tlvs[1].s);

    if (TRACE_APP) {
        printf("[APP] Sending START packet [filesize=%lu,filename=%s,plen=%lu]\n",
            filesize, filename, start_packet.len);
    }

    s = llwrite(fd, start_packet);
    free(start_packet.s);
    return s;
}

int send_end_packet(int fd, size_t filesize, char* filename) {
    int s;
    string tlvs[2];

    s = build_tlv_uint(PCONTROL_TYPE_FILESIZE,
        filesize, tlvs + FILESIZE_TLV_N);
    if (s != 0) return s;

    s = build_tlv_str(PCONTROL_TYPE_FILENAME,
        string_from(filename), tlvs + FILENAME_TLV_N);
    if (s != 0) return s;

    string end_packet;
    s = build_control_packet(PCONTROL_END, tlvs, 2, &end_packet);
    if (s != 0) return s;

    free(tlvs[0].s);
    free(tlvs[1].s);

    if (TRACE_APP) {
        printf("[APP] Sending END packet [filesize=%lu,filename=%s,plen=%lu]\n",
            filesize, filename, end_packet.len);
    }

    s = llwrite(fd, end_packet);
    free(end_packet.s);
    return s;
}

int receive_packet(int fd, data_packet* datap,
        control_packet* controlp) {
    int s;

    string packet;
    s = llread(fd, &packet);
    if (s != 0) return s;

    data_packet data;
    control_packet control;

    if (isDATApacket(packet, &data)) {
        ++in_packet_index;
        *datap = data;

        free(packet.s);
        return PRECEIVE_DATA;
    }

    if (isSTARTpacket(packet, &control)) {
        in_packet_index = 0;
        *controlp = control;

        free(packet.s);
        return PRECEIVE_START;
    }

    if (isENDpacket(packet, &control)) {
        in_packet_index = 0;
        *controlp = control;

        free(packet.s);
        return PRECEIVE_END;
    }

    printf("[APP] Error: Received BAD packet\n");
    free(packet.s);
    return PRECEIVE_BAD_PACKET;
}



