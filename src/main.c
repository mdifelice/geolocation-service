#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <linux/limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BUFFER_SIZE			8192
#define FILE_MAPPING 		"../data/mapping.bin" 
#define FILE_PID	 		"../data/server.pid"
#define LISTEN_PORT			8080
#define LISTEN_QUEUE		20

typedef struct {
	unsigned char	ip_from[16],
					ip_until[16];
	char			*market;
} Mapping;

typedef struct {
	Mapping				*rows;
	unsigned long long	length;
} Array;

typedef struct {
	int				socket;
	unsigned char	ip[16];
} Client;

Array 		mapping;
bool 		running,
			verbose;
char		base_folder[ PATH_MAX ],
			path[ PATH_MAX ];
int			server;
pthread_t	id;
sem_t		semaphore;

void debug( char* message, int error, bool eol ) {
	char 		buffer[ BUFFER_SIZE ],
				*time_description;
	FILE		*output;
	struct tm	*time_info;
	time_t		current_time;
	static bool	last_message_eol = true;

	if( verbose ) {
		time( &current_time );

		time_info = localtime( &current_time );

		if( error ) {
			sprintf( buffer, "%s [ERROR %d]", message, error );

			output = stderr;
		}
		else {
			sprintf( buffer, "%s", message );

			output = stdout;
		}

		if( time_description = asctime( time_info ) )
			time_description[ strlen( time_description ) - 1 ] = '\0';

		if( eol && ! last_message_eol )
			fprintf( output, "\n" );

		fprintf( output,"%s%s: %s%s", eol ? "" : "\r", time_description, buffer, eol ? "\n" : "" );

		last_message_eol = eol;
	}
}

void error( char* content ) {
	debug( content, errno ? errno : -1, true );
}

void message( char* content, ... ) {
	char 	buffer[ BUFFER_SIZE ];
	va_list	args;

	va_start( args, content );

	vsprintf( buffer, content, args );

	va_end( args );

	debug( buffer, 0, true );
}

void progress_message( char* prefix, double progress ) {
	char 		buffer[ BUFFER_SIZE ],
				percentage[ 7 ];
	static char last_percentage[ 7 ] = "";

	sprintf( percentage, "%.1f%%", progress );

	if( strcmp( percentage, last_percentage ) ) {
		strcpy( last_percentage, percentage );

		sprintf( buffer, "%s: %s", prefix, percentage );

		debug( buffer, 0, false );
	}
}

char *get_absolute_path( const char *relative_path ) {
	strcpy( path, base_folder );
	strcat( path, relative_path );

	return path;
}

void free_mapping( Array* mapping ) {
	unsigned long long i;

	for( i = 0; i < mapping->length; i++ ) {
		free( mapping->rows[ i ].market );

		mapping->rows[ i ].market = NULL;
	}

	free( mapping->rows );

	mapping->length	= 0;
	mapping->rows 	= NULL;
}

bool load_mapping() {
	bool				error;
	int					buffer,
						i,
						ip_length;
	char				*file_name;
	Array				temporal;
	unsigned long long	length,
						file_size;
	struct stat			file_info;
	FILE				*fp;

	error 			= false;

	file_size = 0;
	file_name = get_absolute_path( FILE_MAPPING );

	if( file_name ) {
		if( ! stat( file_name, &file_info ) )
			file_size = file_info.st_size;

		message( "Loading mapping" );

		if( fp = fopen( file_name, "r" ) ) {
			temporal.rows 	= NULL;
			temporal.length = 0;

			while( running && ( ip_length = fgetc( fp ) ) != EOF ) {
				buffer = 0;
				length = 0;

				if( ip_length > 32 ) {
					error = true;

					break;
				}

				if( ! ( temporal.rows = ( Mapping* )realloc( temporal.rows, sizeof( Mapping ) * ( temporal.length + 1 ) ) ) ) {
					error = true;

					break;
				}

				if( ! ( temporal.rows[ temporal.length ].market	= ( char* )malloc( sizeof( char ) ) ) ) {
					error = true;

					break;
				}

				bzero( temporal.rows[ temporal.length ].ip_from, 16 );
				bzero( temporal.rows[ temporal.length ].ip_until, 16 );

				if( ip_length == 8 )
					for( i = 10; i <= 11; i++ ) {
						temporal.rows[ temporal.length ].ip_from[ i ] 	= 255;
						temporal.rows[ temporal.length ].ip_until[ i ]	= 255;
					}

				while( running && ( buffer = fgetc( fp ) ) != EOF ) {
					if( length < ip_length / 2 )
						temporal.rows[ temporal.length ].ip_from[ ( length % ( ip_length / 2 ) ) + 16 - ( ip_length / 2 ) ] = buffer;
					else if( length < ip_length )
						temporal.rows[ temporal.length ].ip_until[ ( length % ( ip_length / 2 ) ) + 16 - ( ip_length / 2 ) ] = buffer;
					else if( buffer ) {
						if( ! ( temporal.rows[ temporal.length ].market	= ( char* )realloc( temporal.rows[ temporal.length ].market, sizeof( char ) * ( length - ip_length + 1 ) ) ) ) {
							error = true;

							break;
						}

						temporal.rows[ temporal.length ].market[ length - ip_length ] = buffer;
					}
					else
						break;

					length++;
				}

				if( length >= ip_length ) {
					if( ! ( temporal.rows[ temporal.length ].market	= ( char* )realloc( temporal.rows[ temporal.length ].market, sizeof( char ) * ( length - ip_length + 1 ) ) ) )
						error = true;
					else
						temporal.rows[ temporal.length ].market[ length - ip_length ] = '\0';
				}
				else
					error = true;

				if( error )
					break;

				temporal.length++;

				if( file_size )
					progress_message( "Loaded rows", ( double )( ftell( fp ) * 100 ) / ( double )file_size );
			}

			message( "Finished loading mapping" );

			fclose( fp );

			if( error )
				free_mapping( &temporal );
			else {
				sem_wait( &semaphore );

				free_mapping( &mapping );

				mapping.length 	= temporal.length;
				mapping.rows	= temporal.rows;

				sem_post( &semaphore );
			}
		}
		else
			error = true;
	}
	else
		error = true;

	return ! error;
}

char* find_market( unsigned char* ip ) {
	char				*market,
						buffer[ INET6_ADDRSTRLEN ];
	long long			begin,
						end,
						middle;
	struct sockaddr_in6	info;

	sem_wait( &semaphore );

	market 	= "default";
	begin	= 0;
	end		= mapping.length - 1;

	while( begin <= end ) {
		middle = ( begin + end ) / 2;

		/*
		int i;
		printf( "FROM" );
		for( i = 0; i < 16; i++ )
			printf( " %d", mapping.rows[ middle ].ip_from[ i ] );

		printf( " - UNTIL" );
		for( i = 0; i < 16; i++ )
			printf( " %d", mapping.rows[ middle ].ip_until[ i ] );

		printf( " - IP" );
		for( i = 0; i < 16; i++ )
			printf( " %d", ip[ i ] );
		printf("\n");
		*/

		if( memcmp( mapping.rows[ middle ].ip_until, ip, 16 ) < 0 )
			begin = middle + 1;
		else if( memcmp( mapping.rows[ middle ].ip_from, ip, 16 ) > 0 )
			end = middle - 1;
		else {
			market = mapping.rows[ middle ].market;

			break;
		}
	}

	sem_post( &semaphore );

	memcpy( info.sin6_addr.s6_addr, ip, 16 );

	if( ! inet_ntop( AF_INET6, &info.sin6_addr, buffer, INET6_ADDRSTRLEN ) )
		strcpy( buffer, "Unknown" );

	message( "Client IP %s -> %s", buffer, market );

	return market;
}

bool prepare_server() {
	bool				error;
	int					enable;
	struct sockaddr_in6	info;

	error = false;

	message( "Preparing server" );

	if( ( server = socket( AF_INET6, SOCK_STREAM, 0 ) ) != -1 ) {
		enable = 1;

		if( setsockopt( server, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof( int ) ) == -1 )
			error = true;
		else {
			bzero( &info, sizeof( info ) );

			info.sin6_family 		= AF_INET6;
			info.sin6_port			= htons( LISTEN_PORT );
			info.sin6_addr			= in6addr_any;

			if( bind( server, ( struct sockaddr* )&info, sizeof( info ) ) == -1 )
				error = true;
			else if( listen( server, LISTEN_QUEUE ) == -1 )
				error = true;
		}

		if( error ) {
			close( server );

			server = -1;
		}
	}
	else
		error = true;

	return ! error;
}

Client* get_client() {
	Client				*client;
	int					socket,
						length;
	struct sockaddr_in6	info;

	client = NULL;

	message( "Waiting for clients" );

	length = sizeof( info );

	if(	( socket = accept( server, NULL, NULL ) ) != -1 )
		if( getpeername( socket, ( struct sockaddr* )&info, &length ) != -1 )
			if( client = ( Client* )malloc( sizeof( Client ) ) ) {
				client->socket	= socket;

				memcpy( client->ip, info.sin6_addr.s6_addr, 16 );
			}

	return client;
}

void* process_thread( void* arg ) {
	char 				buffer[ BUFFER_SIZE ],
						*market;
	Client				*client;
	struct pollfd		poll_info;

	client = ( Client* )arg;

	poll_info.fd		= client->socket;
	poll_info.events	= POLLIN;
		
	message( "New client arrives" );

	switch( poll( &poll_info, 1, 1000 ) ) {
		case -1:
			error( "Cannot read from client" );
			break;
		case 0:
			error( "Timeout from client" );
			break;
		default:
			if( read( client->socket, buffer, BUFFER_SIZE ) ) {
				if( ! strncmp( buffer, "GET / ", 6 ) ) {
					market = find_market( client->ip );

					sprintf( buffer, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: max-age=120\r\n\r\n\"%s\"", market );
				}
				else
					sprintf( buffer, "HTTP/1.1 404 Not Found" );

				write( client->socket, buffer, strlen( buffer ) );
			}
	}

	close( client->socket );

	free( client );

	return NULL;
}

void signal_handler( int signal ) {
	static bool reloading = false;

	if( pthread_self() == id ) {
		switch( signal ) {
			case SIGSTOP:
			case SIGINT:
			case SIGTERM:
			case SIGKILL:
				message( "Termination signal received" );

				running = false;

				close( server );
				break;
			case SIGUSR1:
				if( ! reloading ) {
					reloading = true;

					message( "Reloading signal received" );

					free_mapping( &mapping );

					if( ! load_mapping() ) {
						error( "Error loading mapping" );

						running = false;

						close( server );
					}

					reloading = false;
				}
				break;
		}
	}
}

bool start_signal_handler() {
	bool error;

	error = false;

	message( "Attaching signals" );

	if( signal( SIGUSR1, signal_handler ) == SIG_ERR )
		error = true;
	else if( signal( SIGINT, signal_handler ) == SIG_ERR )
		error = true;
	else if( signal( SIGKILL, signal_handler ) == SIG_ERR )
		error = true;
	else if( signal( SIGTERM, signal_handler ) == SIG_ERR )
		error = true;
	else if( signal( SIGSTOP, signal_handler ) == SIG_ERR )
		error = true;

	return error;
}

bool create_pid_file() {
	bool	error;
	FILE* 	fp;

	error = false;

	message( "Creating PID file" );

	if( ! ( fp = fopen( get_absolute_path( FILE_PID ), "w" ) ) )
		error = true;
	else {
		if( ! fprintf( fp, "%d", getpid() ) )
			error = true;

		fclose( fp );
	}

	return ! error;
}

bool get_base_folder( char* executable ) {
	bool 	error;
	char* 	folder;

	error = false;

	if( getcwd( base_folder, PATH_MAX ) ) {
		folder = dirname( executable );

		strcat( base_folder, "/" );
		strcat( base_folder, folder );
		strcat( base_folder, "/" );
	}
	else
		error = true;

	return ! error;
}

int main( int argc, char *argv[] ) {
	Client		*client;
	pthread_t	thread;
	int			i;

	verbose = false;

	for( i = 1; i < argc; i++ )
		if( ! strcmp( argv[ i ], "-v" ) )
			verbose = true;

	message( "Starting" );

	if( ! get_base_folder( argv[0] ) )
		error( "Cannot determine base folder" );
	else {
		if( access( get_absolute_path( FILE_PID ), F_OK ) != -1 )
			error( "Process already running" );
		else {
			sem_init( &semaphore, 0, 1 );

			id 				= pthread_self();
			running			= true;
			mapping.rows	= NULL;
			mapping.length	= 0;

			if( ! start_signal_handler() )
				error( "Cannot start signals" );
			else if( ! create_pid_file() )
				error( "Cannot create PID file" );
			else if( ! prepare_server() )
				error( "Cannot start server" );
			else if( ! load_mapping() )
				error( "Error loading mapping" );
			else {
				while( running ) {
					client = get_client();

					if( ! client )
						error( "Error reading from client" );
					else if( pthread_create( &thread, NULL, &process_thread, client ) )
						error( "Error creating thread" );
				}
			}

			free_mapping( &mapping );

			unlink( get_absolute_path( FILE_PID ) );

			close( server );
		}
	}

	message( "Finished" );

	return errno;
}
