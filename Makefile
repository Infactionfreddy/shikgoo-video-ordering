CC      = cc
CFLAGS  = -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lpthread

# OpenSSL: pkg-config first, then Homebrew fallback (macOS), then plain -lssl
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null || \
                    ([ -d /opt/homebrew/opt/openssl/include ] && \
                     echo -I/opt/homebrew/opt/openssl/include) || \
                    ([ -d /usr/local/opt/openssl/include ] && \
                     echo -I/usr/local/opt/openssl/include))
OPENSSL_LIBS   := $(shell pkg-config --libs openssl 2>/dev/null || \
                    ([ -d /opt/homebrew/opt/openssl/lib ] && \
                     echo -L/opt/homebrew/opt/openssl/lib -lssl -lcrypto) || \
                    ([ -d /usr/local/opt/openssl/lib ] && \
                     echo -L/usr/local/opt/openssl/lib -lssl -lcrypto) || \
                    echo -lssl -lcrypto)

CFLAGS  += $(OPENSSL_CFLAGS)
LDFLAGS += $(OPENSSL_LIBS)

BUILDDIR = build
SRCS     = $(wildcard src/*.c)
OBJS     = $(patsubst src/%.c, $(BUILDDIR)/%.o, $(SRCS))
TARGET   = web_server

all: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(TARGET)

.PHONY: all clean
