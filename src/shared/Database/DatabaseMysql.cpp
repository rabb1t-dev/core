/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <errmsg.h>
#include <mysqld_error.h>
#include "Log.h"
#include "Errors.h"
#include "Util.h"
#include "Policies/SingletonImp.h"
#include "Platform/Define.h"
#include "DatabaseEnv.h"
#include "Timer.h"

size_t DatabaseMysql::db_count = 0;

void DatabaseMysql::ThreadStart()
{
    mysql_thread_init();
}

void DatabaseMysql::ThreadEnd()
{
    mysql_thread_end();
}

DatabaseMysql::DatabaseMysql()
{
    // before first connection
    if (db_count++ == 0)
    {
        // Mysql Library Init
        mysql_library_init(-1, nullptr, nullptr);

        if (!mysql_thread_safe())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "FATAL ERROR: Used MySQL library isn't thread-safe.");
            Log::WaitBeforeContinueIfNeed();
            exit(1);
        }
    }
}

DatabaseMysql::~DatabaseMysql()
{
    StopServer();

    //Free Mysql library pointers for last ~DB
    if(--db_count == 0)
        mysql_library_end();
}

SqlConnection* DatabaseMysql::CreateConnection()
{
    return new MySQLConnection(*this);
}

MySQLConnection::~MySQLConnection()
{
    FreePreparedStatements();
    mysql_close(mMysql);
}

bool MySQLConnection::OpenConnection(bool reconnect)
{
    MYSQL* mysqlInit = mysql_init(nullptr);
    if (!mysqlInit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Could not initialize Mysql connection");
        return false;
    }

    mysql_options(mysqlInit, MYSQL_SET_CHARSET_NAME, "utf8");

    if (m_use_socket)                                           // socket use option (Unix/Linux)
    {
        unsigned int opt = MYSQL_PROTOCOL_SOCKET;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
    }

    mMysql = mysql_real_connect(mysqlInit, m_host.c_str(), m_user.c_str(),
        m_password.c_str(), m_database.c_str(), m_port, nullptr, 0);

    if (mMysql)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "Connected to MySQL database at %s",
            m_host.c_str());
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "MySQL client library: %s", mysql_get_client_info());
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "MySQL server ver: %s ", mysql_get_server_info(mMysql));

        /*----------SET AUTOCOMMIT ON---------*/
        // It seems mysql 5.0.x have enabled this feature
        // by default. In crash case you can lose data!!!
        // So better to turn this off
        // ---
        // This is wrong since mangos use transactions,
        // autocommit is turned of during it.
        // Setting it to on makes atomic updates work
        // ---
        // LEAVE 'AUTOCOMMIT' MODE ALWAYS ENABLED!!!
        // W/O IT EVEN 'SELECT' QUERIES WOULD REQUIRE TO BE WRAPPED INTO 'START TRANSACTION'<>'COMMIT' CLAUSES!!!
        if (!mysql_autocommit(mMysql, 1))
            sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "AUTOCOMMIT SUCCESSFULLY SET TO 1");
        else
            sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "AUTOCOMMIT NOT SET TO 1");
        /*-------------------------------------*/

        // set connection properties to UTF8 to properly handle locales for different
        // server configs - core sends data in UTF8, so MySQL must expect UTF8 too
        Execute("SET NAMES `utf8`");
        Execute("SET CHARACTER SET `utf8`");

        return true;
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Could not connect to MySQL database at %s: %s\n",
            m_host.c_str(), mysql_error(mysqlInit));
        mysql_close(mysqlInit);
        return false;
    }
}

bool MySQLConnection::Reconnect()
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "Reconnection attempt to database %s (on %s)", m_database.c_str(), m_host.c_str());

    if (OpenConnection(true))
    {
        FreePreparedStatements(); // We need to prepare everything again!
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "Successfully reconnected to %s @%s:%u.",
            m_database.c_str(), m_host.c_str(), m_port);

        return true;
    }

    return false; // Failed to reconnect
}

bool MySQLConnection::HandleMySQLError(uint32 errNo)
{
    switch (errNo)
    {
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_LOST:
            #if !(MARIADB_VERSION_ID >= 100200)
        case CR_INVALID_CONN_HANDLE:
            #endif
        case CR_SERVER_LOST_EXTENDED:
        {
            mysql_close(mMysql);
            return Reconnect();
        }

        case ER_LOCK_DEADLOCK:
            return false;
        // Query related errors - skip query
        case ER_WRONG_VALUE_COUNT:
        case ER_DUP_ENTRY:
            return false;

        // The table is there but cannot be read right now: damaged, or held by someone
        // else for longer than we are willing to wait. Neither says anything about the
        // rest of the server, so fail the one query and let its caller decide.
        case ER_CRASHED_ON_USAGE:
        case ER_CRASHED_ON_REPAIR:
        case ER_NOT_KEYFILE:
        case ER_LOCK_WAIT_TIMEOUT:
            sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Table temporarily unusable (errno %u). Query abandoned.", errNo);
            return false;

        // Outdated table or database structure - terminate core
        case ER_BAD_FIELD_ERROR:
        case ER_NO_SUCH_TABLE:
            sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Your database structure is not up to date. Please make sure you have executed all the queries in the sql/updates folders.");
            ASSERT(false);
            return false;
        case ER_PARSE_ERROR:
            sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Error while parsing SQL. Core fix required.");
            ASSERT(false);
            return false;
        default:
            // Deliberately not fatal. This runs on the database worker thread, which has no
            // handler for the exception an assertion throws, so asserting here takes the
            // whole world down; an error nobody has classified yet is a poor reason to do
            // that to everyone online. Abandoning the query leaves the caller to notice.
            sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Unhandled MySQL errno %u. Query abandoned.", errNo);
            return false;
    }
}

bool MySQLConnection::_Query(std::string const& sql, MYSQL_RES** pResult, MYSQL_FIELD** pFields, uint64* pRowCount, uint32* pFieldCount, bool* pFailed)
{
    // Assumed broken until the query is known to have run. Every path out of here that is
    // an ordinary empty result says so explicitly.
    if (pFailed)
        *pFailed = true;

    if (!mMysql && !Reconnect())
        return false;

    uint32 _s = WorldTimer::getMSTime();

    if (mysql_query(mMysql, sql.c_str()))
    {
        uint32 lErrno = mysql_errno(mMysql);

        sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "SQL: %s", sql.c_str());
        sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "[%u] %s", lErrno, mysql_error(mMysql));

        if (HandleMySQLError(lErrno)) // If error is handled, just try again
            return _Query(sql, pResult, pFields, pRowCount, pFieldCount, pFailed);

        return false;
    }
    else
    {
        DEBUG_FILTER_LOG(LOG_FILTER_SQL_TEXT, "[%u ms] SQL: %s", WorldTimer::getMSTimeDiff(_s,WorldTimer::getMSTime()), sql.c_str());
    }

    *pResult = mysql_store_result(mMysql);
    *pRowCount = mysql_affected_rows(mMysql);
    *pFieldCount = mysql_field_count(mMysql);

    if (!*pResult)
    {
        // A null result set means the statement produced none, which is normal for anything
        // that is not a SELECT, unless the server said it had fields to return: then the
        // set went missing between the query and the fetch.
        if (pFailed && !*pFieldCount)
            *pFailed = false;
        return false;
    }

    if (!*pRowCount)
    {
        mysql_free_result(*pResult);
        if (pFailed)
            *pFailed = false;
        return false;
    }

    *pFields = mysql_fetch_fields(*pResult);
    if (pFailed)
        *pFailed = false;
    return true;
}

bool MySQLConnection::QueryChecked(std::string const& sql, std::unique_ptr<QueryResult>& result)
{
    MYSQL_RES* mysqlResult = nullptr;
    MYSQL_FIELD* fields = nullptr;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;
    bool failed = false;

    result = nullptr;

    if (!_Query(sql, &mysqlResult, &fields, &rowCount, &fieldCount, &failed))
        return !failed;

    std::unique_ptr<QueryResultMysql> queryResult(new QueryResultMysql(mysqlResult, fields, rowCount, fieldCount));

    queryResult->NextRow();
    result = std::move(queryResult);
    return true;
}

std::unique_ptr<QueryResult> MySQLConnection::Query(std::string const& sql)
{
    std::unique_ptr<QueryResult> result;
    QueryChecked(sql, result);
    return result;
}

std::unique_ptr<QueryNamedResult> MySQLConnection::QueryNamed(std::string const& sql)
{
    MYSQL_RES* result = nullptr;
    MYSQL_FIELD* fields = nullptr;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;

    if(!_Query(sql, &result, &fields, &rowCount, &fieldCount))
        return nullptr;

    QueryFieldNames names(fieldCount);
    for (uint32 i = 0; i < fieldCount; i++)
        names[i] = fields[i].name;

    std::unique_ptr<QueryResultMysql> queryResult(new QueryResultMysql(result, fields, rowCount, fieldCount));

    queryResult->NextRow();
    return std::make_unique<QueryNamedResult>(std::move(queryResult), names);
}

bool MySQLConnection::Execute(std::string const& sql)
{
    if (!mMysql)
        return false;

    uint32 _s = WorldTimer::getMSTime();

    if (mysql_query(mMysql, sql.c_str()))
    {
        uint32 lErrno = mysql_errno(mMysql);

        sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "SQL: %s", sql.c_str());
        sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "[%u] %s", lErrno, mysql_error(mMysql));

        if (HandleMySQLError(lErrno)) // If error is handled, just try again
            return Execute(sql);
        return false;
    }
    else
    {
        DEBUG_FILTER_LOG(LOG_FILTER_SQL_TEXT, "[%u ms] SQL: %s", WorldTimer::getMSTimeDiff(_s,WorldTimer::getMSTime()), sql.c_str());
    }

    return true;
}

bool MySQLConnection::_TransactionCmd(std::string const& sql)
{
    if (mysql_query(mMysql, sql.c_str()))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL: %s", sql.c_str());
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL ERROR: %s", mysql_error(mMysql));
        return false;
    }
    else
    {
        DEBUG_FILTER_LOG(LOG_FILTER_SQL_TEXT, "SQL: %s", sql.c_str());
    }
    return true;
}

bool MySQLConnection::BeginTransaction()
{
    return _TransactionCmd("START TRANSACTION");
}

bool MySQLConnection::CommitTransaction()
{
    return _TransactionCmd("COMMIT");
}

bool MySQLConnection::RollbackTransaction()
{
    return _TransactionCmd("ROLLBACK");
}

unsigned long MySQLConnection::escape_string(char* to, char const* from, unsigned long length)
{
    if (!mMysql || !to || !from || !length)
        return 0;

    return mysql_real_escape_string(mMysql, to, from, length);
}

//////////////////////////////////////////////////////////////////////////
SqlPreparedStatement* MySQLConnection::CreateStatement(std::string const& fmt)
{
    return new MySqlPreparedStatement(fmt, *this, mMysql);
}


//////////////////////////////////////////////////////////////////////////
MySqlPreparedStatement::MySqlPreparedStatement(std::string const& fmt, SqlConnection& conn, MYSQL* mysql) : SqlPreparedStatement(fmt, conn),
    m_pMySQLConn(mysql), m_stmt(nullptr), m_pInputArgs(nullptr), m_pResult(nullptr), m_pResultMetadata(nullptr)
{
}

MySqlPreparedStatement::~MySqlPreparedStatement()
{
    RemoveBinds();
}

bool MySqlPreparedStatement::prepare()
{
    if(isPrepared())
        return true;

    //remove old binds
    RemoveBinds();

    //create statement object
    m_stmt = mysql_stmt_init(m_pMySQLConn);
    if (!m_stmt)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL: mysql_stmt_init()() failed ");
        return false;
    }

    //prepare statement
    if (mysql_stmt_prepare(m_stmt, m_szFmt.c_str(), m_szFmt.length()))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL: mysql_stmt_prepare() failed for '%s'", m_szFmt.c_str());
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL ERROR: %s", mysql_stmt_error(m_stmt));
        return false;
    }

    /* Get the parameter count from the statement */
    m_nParams = mysql_stmt_param_count(m_stmt);

    /* Fetch result set meta information */
    m_pResultMetadata = mysql_stmt_result_metadata(m_stmt);
    //if we do not have result metadata
    if (!m_pResultMetadata && StringStartsWithCaseInsensitive(m_szFmt, "select"))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL: no meta information for '%s'", m_szFmt.c_str());
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL ERROR: %s", mysql_stmt_error(m_stmt));
        return false;
    }

    //bind input buffers
    if(m_nParams)
    {
        m_pInputArgs = new MYSQL_BIND[m_nParams];
        memset(m_pInputArgs, 0, sizeof(MYSQL_BIND) * m_nParams);
    }

    //check if we have a statement which returns result sets
    if(m_pResultMetadata)
    {
        //our statement is query
        m_bIsQuery = true;
        /* Get total columns in the query */
        m_nColumns = mysql_num_fields(m_pResultMetadata);

        //bind output buffers
    }

    m_bPrepared = true;
    return true;
}

void MySqlPreparedStatement::bind(SqlStmtParameters const& holder)
{
    if(!isPrepared())
    {
        MANGOS_ASSERT(false);
        return;
    }

    //finalize adding params
    if(!m_pInputArgs)
        return;

    //verify if we bound all needed input parameters
    if(m_nParams != holder.boundParams())
    {
        MANGOS_ASSERT(false);
        return;
    }

    int nIndex = 0;
    SqlStmtParameters::ParameterContainer const& _args = holder.params();

    SqlStmtParameters::ParameterContainer::const_iterator iter_last = _args.end();
    for (SqlStmtParameters::ParameterContainer::const_iterator iter = _args.begin(); iter != iter_last; ++iter)
    {
        //bind parameter
        addParam(nIndex++, (*iter));
    }

    //bind input arguments
    if (mysql_stmt_bind_param(m_stmt, m_pInputArgs))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL ERROR: mysql_stmt_bind_param() failed\n");
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL ERROR: %s", mysql_stmt_error(m_stmt));
    }
}

void MySqlPreparedStatement::addParam(int nIndex, SqlStmtFieldData const& data)
{
    MANGOS_ASSERT(m_pInputArgs);
    MANGOS_ASSERT(nIndex < static_cast <int32> (m_nParams));

    MYSQL_BIND& pData = m_pInputArgs[nIndex];

    my_bool bUnsigned = 0;
    enum_field_types dataType = ToMySQLType(data, bUnsigned);

    //setup MYSQL_BIND structure
    pData.buffer_type = dataType;
    pData.is_unsigned = bUnsigned;
    pData.buffer = data.buff();
    pData.length = 0;
    pData.buffer_length = data.type() == FIELD_STRING ? data.size() : 0;
}

void MySqlPreparedStatement::RemoveBinds()
{
    if(!m_stmt)
        return;

    delete [] m_pInputArgs;
    delete [] m_pResult;

    mysql_free_result(m_pResultMetadata);
    mysql_stmt_close(m_stmt);

    m_stmt = nullptr;
    m_pResultMetadata = nullptr;
    m_pResult = nullptr;
    m_pInputArgs = nullptr;

    m_bPrepared = false;
}

bool MySqlPreparedStatement::execute()
{
    if(!isPrepared())
        return false;

    if(mysql_stmt_execute(m_stmt))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL: cannot execute '%s'", m_szFmt.c_str());
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "SQL ERROR: %s", mysql_stmt_error(m_stmt));
        return false;
    }

    return true;
}

enum_field_types MySqlPreparedStatement::ToMySQLType(SqlStmtFieldData const& data, my_bool& bUnsigned)
{
    bUnsigned = 0;
    enum_field_types dataType = MYSQL_TYPE_NULL;

    switch (data.type())
    {
    case FIELD_NONE:    dataType = MYSQL_TYPE_NULL;                     break;
    case FIELD_BOOL:    dataType = MYSQL_TYPE_BIT;      bUnsigned = 1;  break;
    case FIELD_I8:      dataType = MYSQL_TYPE_TINY;                     break;
    case FIELD_UI8:     dataType = MYSQL_TYPE_TINY;     bUnsigned = 1;  break;
    case FIELD_I16:     dataType = MYSQL_TYPE_SHORT;                    break;
    case FIELD_UI16:    dataType = MYSQL_TYPE_SHORT;    bUnsigned = 1;  break;
    case FIELD_I32:     dataType = MYSQL_TYPE_LONG;                     break;
    case FIELD_UI32:    dataType = MYSQL_TYPE_LONG;     bUnsigned = 1;  break;
    case FIELD_I64:     dataType = MYSQL_TYPE_LONGLONG;                 break;
    case FIELD_UI64:    dataType = MYSQL_TYPE_LONGLONG; bUnsigned = 1;  break;
    case FIELD_FLOAT:   dataType = MYSQL_TYPE_FLOAT;                    break;
    case FIELD_DOUBLE:  dataType = MYSQL_TYPE_DOUBLE;                   break;
    case FIELD_STRING:  dataType = MYSQL_TYPE_STRING;                   break;
    }

    return dataType;
}
