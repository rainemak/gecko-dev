/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZ_EMBED_LOG_H
#define MOZ_EMBED_LOG_H

#include <stdio.h>

#define PR_LOGGING 1

#ifdef PR_LOGGING

//#ifdef EMBED_LITE_INTERNAL
#if 1

#include <stdio.h>
#include "prlog.h"

extern PRLogModuleInfo* GetEmbedCommonLog(const char* aModule);

#define LOGF(FMT, ...) printf("FUNC::%s:%d \n" FMT , __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOGT(FMT, ...) printf("TRACE::%s:%d \n" FMT , __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOGW(FMT, ...) printf("WARN: EmbedLite::%s:%d \n" FMT , __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOGE(FMT, ...) printf("ERROR: EmbedLite::%s:%d \n" FMT , __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOGNI(FMT, ...) printf("NON_IMPL: EmbedLite::%s:%d \n" FMT , __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)

#define LOGC(CUSTOMNAME, FMT, ...) printf("::%s:%d \n" FMT , __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)

#else // EMBED_LITE_INTERNAL

#define LOGT(FMT, ...) fprintf(stderr, \
                               "EmbedLiteExt %s:%d: " FMT "\n", __PRETTY_FUNCTION__, __LINE__, ##__VA_ARGS__)

#endif // EMBED_LITE_INTERNAL

#else // PR_LOGGING

#define LOGF(...) do {} while (0)
#define LOGT(...) do {} while (0)
#define LOGW(...) do {} while (0)
#define LOGE(...) do {} while (0)
#define LOGC(...) do {} while (0)

#endif // PR_LOGGING

#endif // MOZ_EMBED_LOG_H
