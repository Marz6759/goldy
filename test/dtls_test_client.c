/*
 * dtls_test_client.c -
 */

#if defined(__linux__)
#define _XOPEN_SOURCE 700
#endif

#include "mbedtls/config.h"
#include "mbedtls/platform.h"

#include "mbedtls/net.h"
#include "mbedtls/debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"
#include "mbedtls/timing.h"

#include <getopt.h>
#include <string.h>

#define READ_TIMEOUT_MS 2000
#define MAX_RETRY       5

#define DEBUG_LEVEL 0

static void my_debug( void *ctx, int level,
                      const char *file, int line,
                      const char *str )
{
    ((void) level);

    fprintf( (FILE *) ctx, "%s:%04d: %s", file, line, str );
    fflush(  (FILE *) ctx  );
}

void print_usage(const char* argv0) {
    printf("Usage: %s -h host -p port [-n ssl_hostname] -b packet_body\n", argv0);
    exit(1);
}

int main( int argc, char *argv[] )
{
    int ret, len;
    mbedtls_net_context server_fd;
    uint32_t flags;
    unsigned char buf[10000];
    char packet_body[10000] = "";
    char server_host[100] = "";
    char server_port[6] = "";
    char server_ssl_hostname[100] = "";
    const char *pers = "dtls_client";
    int retry_left = MAX_RETRY;
    int opt;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_timing_delay_context timer;

    /* Parse command line */
    while ((opt = getopt(argc, argv, "b:h:n:p:")) != -1) {
        switch (opt) {
        case 'b':
            strncpy(packet_body, optarg, sizeof(packet_body));
            break;
        case 'h':
            strncpy(server_host, optarg, sizeof(server_host));
            break;
        case 'n':
            strncpy(server_ssl_hostname, optarg, sizeof(server_ssl_hostname));
            break;
        case 'p':
            strncpy(server_port, optarg, sizeof(server_port));
            break;
        default: /* '?' */
            print_usage(argv[0]);
        }
    }

    if (!(packet_body[0] && server_port[0] && server_host[0])) {
        print_usage(argv[0]);
    }

    if (!server_ssl_hostname[0]) {
        strncpy(server_ssl_hostname, server_host, sizeof(server_ssl_hostname));
    }

#if defined(MBEDTLS_DEBUG_C)
    mbedtls_debug_set_threshold( DEBUG_LEVEL );
#endif

    /*
     * 0. Initialize the RNG and the session data
     */
    mbedtls_net_init( &server_fd );
    mbedtls_ssl_init( &ssl );
    mbedtls_ssl_config_init( &conf );
    mbedtls_x509_crt_init( &cacert );
    mbedtls_ctr_drbg_init( &ctr_drbg );

    printf( "dtls_test_client: Seeding the random number generator...\n" );
    fflush( stdout );

    mbedtls_entropy_init( &entropy );
    if( ( ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *) pers,
                               strlen( pers ) ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret );
        goto exit;
    }

    printf( "dtls_test_client: ok\n" );

    /*
     * 0. Load certificates
     */
    printf( "dtls_test_client: Loading the CA root certificate ...\n" );
    fflush( stdout );

    ret = mbedtls_x509_crt_parse( &cacert, (const unsigned char *) mbedtls_test_cas_pem,
                          mbedtls_test_cas_pem_len );
    if( ret < 0 )
    {
        printf( " failed\n  !  mbedtls_x509_crt_parse returned -0x%x\n\n", -ret );
        goto exit;
    }

    printf( "dtls_test_client: ok (%d skipped)\n", ret );

    /*
     * 1. Start the connection
     */
    printf( "dtls_test_client: Connecting to udp %s:%s (SSL hostname: %s)...\n", server_host, server_port, server_ssl_hostname);
    fflush( stdout );

    if ((ret = mbedtls_net_connect(&server_fd, server_host, server_port, MBEDTLS_NET_PROTO_UDP)) != 0)
    {
        printf( " failed\n  ! mbedtls_net_connect returned %d\n\n", ret );
        goto exit;
    }

    printf( "dtls_test_client: ok\n" );

    /*
     * 2. Setup stuff
     */
    printf( "dtls_test_client: Setting up the DTLS structure...\n" );
    fflush( stdout );

    if( ( ret = mbedtls_ssl_config_defaults( &conf,
                   MBEDTLS_SSL_IS_CLIENT,
                   MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                   MBEDTLS_SSL_PRESET_DEFAULT ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret );
        goto exit;
    }

    /* OPTIONAL is usually a bad choice for security, but makes interop easier
     * in this simplified example, in which the ca chain is hardcoded.
     * Production code should set a proper ca chain and use REQUIRED. */
    mbedtls_ssl_conf_authmode( &conf, MBEDTLS_SSL_VERIFY_OPTIONAL );
    mbedtls_ssl_conf_ca_chain( &conf, &cacert, NULL );
    mbedtls_ssl_conf_rng( &conf, mbedtls_ctr_drbg_random, &ctr_drbg );
    mbedtls_ssl_conf_dbg( &conf, my_debug, stdout ); /* TODO remove */
    /* TODO timeouts */

    if( ( ret = mbedtls_ssl_setup( &ssl, &conf ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret );
        goto exit;
    }

    if( ( ret = mbedtls_ssl_set_hostname( &ssl, server_ssl_hostname ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_set_hostname returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_ssl_set_bio( &ssl, &server_fd,
                         mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout );

    mbedtls_ssl_set_timer_cb( &ssl, &timer, mbedtls_timing_set_delay,
                                            mbedtls_timing_get_delay );

    printf( "dtls_test_client: ok\n" );

    /*
     * 4. Handshake
     */
    printf( "dtls_test_client: Performing the SSL/TLS handshake...\n" );
    fflush( stdout );

    do ret = mbedtls_ssl_handshake( &ssl );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    if( ret != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_handshake returned -0x%x\n\n", -ret );
        goto exit;
    }

    printf( "dtls_test_client: ok\n" );

    /*
     * 5. Verify the server certificate
     */
    printf( "dtls_test_client: Verifying peer X.509 certificate...\n" );

    /* In real life, we would have used MBEDTLS_SSL_VERIFY_REQUIRED so that the
     * handshake would not succeed if the peer's cert is bad.  Even if we used
     * MBEDTLS_SSL_VERIFY_OPTIONAL, we would bail out here if ret != 0 */
    if( ( flags = mbedtls_ssl_get_verify_result( &ssl ) ) != 0 )
    {
        char vrfy_buf[512];

        printf( "dtls_test_client: failed\n" );

        mbedtls_x509_crt_verify_info( vrfy_buf, sizeof( vrfy_buf ), "dtls_test_client: ! ", flags );

        printf( "%s", vrfy_buf );
    }
    else
        printf( "dtls_test_client: ok\n" );

    /*
     * 6. Write the echo request
     */
send_request:
    printf( "dtls_test_client: Write to server:\n" );
    fflush( stdout );

    len = strlen(packet_body);

    do ret = mbedtls_ssl_write( &ssl, (unsigned char *) packet_body, len );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    if( ret < 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_write returned %d\n\n", ret );
        goto exit;
    }

    len = ret;
    printf( "dtls_test_client: %d bytes written: '%s'\n", len, packet_body );

    /*
     * 7. Read the echo response
     */
    printf( "dtls_test_client: Read from server:\n" );
    fflush( stdout );

    len = sizeof( buf ) - 1;
    memset( buf, 0, sizeof( buf ) );

    do ret = mbedtls_ssl_read( &ssl, buf, len );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    if( ret <= 0 )
    {
        switch( ret )
        {
            case MBEDTLS_ERR_SSL_TIMEOUT:
                printf( "dtls_test_client:  timeout\n\n" );
                if( retry_left-- > 0 )
                    goto send_request;
                goto exit;

            case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
                printf( "dtls_test_client:  connection was closed gracefully\n" );
                ret = 0;
                goto close_notify;

            default:
                printf( "dtls_test_client:  mbedtls_ssl_read returned -0x%x\n\n", -ret );
                goto exit;
        }
    }

    len = ret;
    printf( "dtls_test_client: %d bytes read: '%s'\n", len, buf );
    fflush(stdout);

    /*
     * 8. Done, cleanly close the connection
     */
close_notify:
    printf( "dtls_test_client: Closing the connection...\n" );

    /* No error checking, the connection might be closed already */
    do ret = mbedtls_ssl_close_notify( &ssl );
    while( ret == MBEDTLS_ERR_SSL_WANT_WRITE );
    ret = 0;

    printf( "dtls_test_client: done\n" );

    /*
     * 9. Final clean-ups and exit
     */
exit:

#ifdef MBEDTLS_ERROR_C
    if( ret != 0 )
    {
        char error_buf[100];
        mbedtls_strerror( ret, error_buf, 100 );
        printf( "Last error was: %d - %s\n\n", ret, error_buf );
    }
#endif

    mbedtls_net_free( &server_fd );

    mbedtls_x509_crt_free( &cacert );
    mbedtls_ssl_free( &ssl );
    mbedtls_ssl_config_free( &conf );
    mbedtls_ctr_drbg_free( &ctr_drbg );
    mbedtls_entropy_free( &entropy );

    return ret == 0 ? 0 : 1;
}