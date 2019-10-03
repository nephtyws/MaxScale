/**
 * @file connection_limit.cpp  connection_limit check if max_connections parameter works
 *
 * - Maxscale.cnf contains max_connections=10 for RWSplit, max_connections=20 for ReadConn master and
 * max_connections=25 for ReadConn slave
 * - create max num of connections and check tha N+1 connection fails
 */

#include "testconnections.h"

void check_with_wrong_pw(int router, int max_conn, TestConnections& test);
void check_max_conn(int router, int max_conn, TestConnections& test);

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);

    // First test with wrong pw to see that count is properly decremented.
    test.tprintf("Trying 20 connections with RWSplit with wrong password\n");
    check_with_wrong_pw(0, 20, test);

    if (test.ok())
    {
        test.tprintf("Trying 11 connections with RWSplit\n");
        check_max_conn(0, 10, test);
    }

    if (test.ok())
    {
        test.tprintf("Trying 21 connections with Readconn master\n");
        check_max_conn(1, 20, test);
    }

    if (test.ok())
    {
        test.tprintf("Trying 26 connections with Readconnn slave\n");
        check_max_conn(2, 25, test);
    }

    test.check_maxscale_alive(0);
    int rval = test.global_result;
    return rval;
}

void check_with_wrong_pw(int router, int max_conn, TestConnections& test)
{
    // Check MXS-2645, connection count is not decremented properly when authentication fails.
    const char wrong_pw[] = "batman";
    bool limit_reached = false;

    for (int i = 0; i < max_conn && !limit_reached; i++)
    {
        MYSQL* failed_conn = open_conn(
            test.maxscales->ports[0][router], test.maxscales->IP[0],
            test.maxscales->user_name, wrong_pw,
            test.ssl);
        auto error = mysql_errno(failed_conn);
        if (error == 0)
        {
            test.expect(false, "Connection succeeded when it should have failed.");
        }
        else if (error == 1040)
        {
            test.expect(false, "Connection limit wrongfully reached.");
            limit_reached = true;
        }
        mysql_close(failed_conn);
    }
}

void check_max_conn(int router, int max_conn, TestConnections& test)
{
    MYSQL* conn[max_conn + 1];

    int i;
    for (i = 0; i < max_conn; i++)
    {
        conn[i] = open_conn(test.maxscales->ports[0][router],
                            test.maxscales->IP[0],
                            test.maxscales->user_name,
                            test.maxscales->password,
                            test.ssl);
        if (mysql_errno(conn[i]) != 0)
        {
            test.add_result(1, "Connection %d failed, error is %s\n", i, mysql_error(conn[i]));
        }
    }
    conn[max_conn] = open_conn(test.maxscales->ports[0][router],
                               test.maxscales->IP[0],
                               test.maxscales->user_name,
                               test.maxscales->password,
                               test.ssl);
    if (mysql_errno(conn[i]) != 1040)
    {
        test.add_result(1,
                         "Max_xonnections reached, but error is not 1040, it is %d %s\n",
                         mysql_errno(conn[i]),
                         mysql_error(conn[i]));
    }
    for (i = 0; i < max_conn; i++)
    {
        mysql_close(conn[i]);
    }
}
