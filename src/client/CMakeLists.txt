cmake_minimum_required(VERSION 3.10)
project(client)

set(SRC
	WFDnsClient.cc
	WFHttpChunkedClient.cc
)

if (NOT REDIS STREQUAL "n")
	set(SRC
		${SRC}
		WFRedisSubscriber.cc
	)
endif ()

if (NOT MYSQL STREQUAL "n")
	set(SRC
		${SRC}
		WFMySQLConnection.cc
	)
endif ()

if (NOT CONSUL STREQUAL "n")
	set(SRC
		${SRC}
		WFConsulClient.cc
	)
endif ()

add_library(${PROJECT_NAME} OBJECT ${SRC})

if (KAFKA STREQUAL "y")
	set(SRC
		WFKafkaClient.cc
	)
	add_library("client_kafka" OBJECT ${SRC})
endif ()

