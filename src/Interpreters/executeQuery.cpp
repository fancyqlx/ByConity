/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#include <memory>
#include <Client/Connection.h>
#include <Interpreters/executeQueryHelper.h>
#include <Common/Config/VWCustomizedSettings.h>
#include <Common/Exception.h>
#include <Common/HostWithPorts.h>
#include <Common/PODArray.h>
#include <Common/ThreadProfileEvents.h>
#include <Common/formatReadable.h>
#include <Common/typeid_cast.h>
#include <common/logger_useful.h>
#include <common/types.h>

#include <IO/LimitReadBuffer.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromVector.h>
#include <IO/ZlibDeflatingWriteBuffer.h>
#include <IO/copyData.h>
#include <Storages/HDFS/WriteBufferFromHDFS.h>

#include <DataStreams/BlockIO.h>
#include <DataStreams/CountingBlockOutputStream.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/RemoteBlockInputStream.h>
#include <DataStreams/RemoteBlockOutputStream.h>
#include <DataStreams/copyData.h>
#include <Processors/Sources/RemoteSource.h>
#include <Processors/Transforms/getSourceFromFromASTInsertQuery.h>

#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTShowProcesslistQuery.h>
#include <Parsers/ASTWatchQuery.h>
#include <Parsers/ASTExplainQuery.h>
#include <Parsers/Lexer.h>
#include <Parsers/ParserQuery.h>
#include <Parsers/parseQuery.h>
#include <Parsers/queryNormalization.h>
#include <Parsers/queryToString.h>

#include <Formats/FormatFactory.h>
#include <Storages/StorageInput.h>

#include <Access/EnabledQuota.h>
#include <Interpreters/ApplyWithGlobalVisitor.h>
#include <Interpreters/CnchQueryMetrics/QueryMetricLogHelper.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterFactory.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/InterpreterSelectQueryUseOptimizer.h>
#include <Interpreters/InterpreterSetQuery.h>
#include <Interpreters/NormalizeSelectWithUnionQueryVisitor.h>
#include <Interpreters/OpenTelemetrySpanLog.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/ProcessorsProfileLog.h>
#include <Interpreters/QueryLog.h>
#include <Interpreters/QueueManager.h>
#include <Interpreters/ReplaceQueryParameterVisitor.h>
#include <Interpreters/SelectIntersectExceptQueryVisitor.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Interpreters/VirtualWarehouseHandle.h>
#include <Interpreters/executeQuery.h>
#include <Interpreters/trySetVirtualWarehouse.h>
#include <Interpreters/Cache/QueryCache.h>

#include <Common/ProfileEvents.h>
#include <Common/RpcClientPool.h>

#include <DataStreams/IBlockStream_fwd.h>
#include <DataStreams/NullBlockOutputStream.h>
#include <DataStreams/RemoteQueryExecutor.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteBufferFromString.h>
#include <Interpreters/Cluster.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/InterpreterCommitQuery.h>
#include <Interpreters/NamedSession.h>
#include <Interpreters/Set.h>
#include <Interpreters/StorageID.h>
#include <Interpreters/trySetVirtualWarehouse.h>
#include <MergeTreeCommon/CnchTopologyMaster.h>
#include <Parsers/IAST_fwd.h>
#include <Processors/QueryPipeline.h>
#include <Storages/StorageCloudMergeTree.h>
#include <Transaction/CnchWorkerTransaction.h>
#include <Transaction/TransactionCoordinatorRcCnch.h>
#include <Transaction/TxnTimestamp.h>
#include <Common/SensitiveDataMasker.h>

#include <Interpreters/DistributedStages/MPPQueryCoordinator.h>
#include <Interpreters/DistributedStages/MPPQueryManager.h>
#include <Interpreters/DistributedStages/PlanSegmentExecutor.h>
#include <Interpreters/InterpreterPerfectShard.h>


#include <Processors/Formats/IOutputFormat.h>
#include <Processors/Sources/SinkToOutputStream.h>
#include <Processors/Sources/SourceFromInputStream.h>

#include <Processors/Transforms/LimitsCheckingTransform.h>
#include <Processors/Transforms/MaterializingTransform.h>

#include <QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <QueryPlan/ReadFromPreparedSource.h>

#include <Interpreters/RuntimeFilter/RuntimeFilterManager.h>

#include <Transaction/ICnchTransaction.h>
#include <Transaction/TransactionCoordinatorRcCnch.h>

#include <Optimizer/OptimizerMetrics.h>
#include <Optimizer/QueryUseOptimizerChecker.h>
#include <Protos/cnch_common.pb.h>
#include <Optimizer/OptimizerMetrics.h>

using AsyncQueryStatus = DB::Protos::AsyncQueryStatus;

namespace ProfileEvents
{
extern const Event QueryMaskingRulesMatch;
extern const Event FailedQuery;
extern const Event FailedInsertQuery;
extern const Event FailedSelectQuery;
extern const Event QueryTimeMicroseconds;
extern const Event SelectQueryTimeMicroseconds;
extern const Event InsertQueryTimeMicroseconds;
}

namespace DB
{
namespace ErrorCodes
{
    extern const int INTO_OUTFILE_NOT_ALLOWED;
    extern const int QUERY_WAS_CANCELLED;
    extern const int CANNOT_PARSE_DOMAIN_VALUE_FROM_STRING;
    extern const int CNCH_QUEUE_QUERY_FAILURE;
}

void tryQueueQuery(ContextMutablePtr context, ASTType ast_type)
{
    auto worker_group_handler = context->tryGetCurrentWorkerGroup();
    if (ast_type != ASTType::ASTSelectQuery && ast_type != ASTType::ASTSelectWithUnionQuery && ast_type != ASTType::ASTInsertQuery
        && ast_type != ASTType::ASTDeleteQuery && ast_type != ASTType::ASTUpdateQuery)
    {
        LOG_DEBUG(&Poco::Logger::get("executeQuery"), "only queue dml query");
        return;
    }
    if (worker_group_handler)
    {
        Stopwatch queue_watch;
        queue_watch.start();
        auto query_queue = context->getQueueManager();
        auto query_id = context->getCurrentQueryId();
        const auto & vw_name = worker_group_handler->getVWName();
        const auto & wg_name = worker_group_handler->getID();
        context->getWorkerStatusManager()->updateVWWorkerList(worker_group_handler->getHostWithPortsVec(), vw_name, wg_name);
        auto queue_info = std::make_shared<QueueInfo>(query_id, vw_name, wg_name, context);
        auto queue_result = query_queue->enqueue(queue_info, context->getSettingsRef().query_queue_timeout_ms);
        if (queue_result == QueueResultStatus::QueueSuccess)
        {
            auto current_vw = context->tryGetCurrentVW();
            if (current_vw)
            {
                context->setCurrentWorkerGroup(current_vw->getWorkerGroup(wg_name));
            }
            LOG_DEBUG(&Poco::Logger::get("executeQuery"), "query queue run time : {} ms", queue_watch.elapsedMilliseconds());
        }
        else
        {
            LOG_ERROR(&Poco::Logger::get("executeQuery"), "query queue result : {}", queueResultStatusToString(queue_result));
            throw Exception(
                ErrorCodes::CNCH_QUEUE_QUERY_FAILURE,
                "query queue failed for query_id {}: {}",
                query_id,
                queueResultStatusToString(queue_result));
        }
    }
}

static void checkASTSizeLimits(const IAST & ast, const Settings & settings)
{
    if (settings.max_ast_depth)
        ast.checkDepth(settings.max_ast_depth);
    if (settings.max_ast_elements)
        ast.checkSize(settings.max_ast_elements);
}


static String joinLines(const String & query)
{
    /// Care should be taken. We don't join lines inside non-whitespace tokens (e.g. multiline string literals)
    ///  and we don't join line after comment (because it can be single-line comment).
    /// All other whitespaces replaced to a single whitespace.

    String res;
    const char * begin = query.data();
    const char * end = begin + query.size();

    Lexer lexer(begin, end);
    Token token = lexer.nextToken();
    for (; !token.isEnd(); token = lexer.nextToken())
    {
        if (token.type == TokenType::Whitespace)
        {
            res += ' ';
        }
        else if (token.type == TokenType::Comment)
        {
            res.append(token.begin, token.end);
            if (token.end < end && *token.end == '\n')
                res += '\n';
        }
        else
            res.append(token.begin, token.end);
    }

    return res;
}


static String prepareQueryForLogging(const String & query, ContextPtr context)
{
    String res = query;

    // wiping sensitive data before cropping query by log_queries_cut_to_length,
    // otherwise something like credit card without last digit can go to log
    if (auto * masker = SensitiveDataMasker::getInstance())
    {
        auto matches = masker->wipeSensitiveData(res);
        if (matches > 0)
        {
            ProfileEvents::increment(ProfileEvents::QueryMaskingRulesMatch, matches);
        }
    }

    res = res.substr(0, context->getSettingsRef().log_queries_cut_to_length);

    return res;
}


/// Log query into text log (not into system table).
static void logQuery(const String & query, ContextPtr context, bool internal)
{
    if (internal)
    {
        LOG_DEBUG(&Poco::Logger::get("executeQuery"), "(internal) {}", joinLines(query));
    }
    else
    {
        const auto & client_info = context->getClientInfo();

        const auto & current_query_id = client_info.current_query_id;
        const auto & initial_query_id = client_info.initial_query_id;
        const auto & current_user = client_info.current_user;

        String comment = context->getSettingsRef().log_comment;
        size_t max_query_size = context->getSettingsRef().max_query_size;

        if (comment.size() > max_query_size)
            comment.resize(max_query_size);

        if (!comment.empty())
            comment = fmt::format(" (comment: {})", comment);

        LOG_DEBUG(
            &Poco::Logger::get("executeQuery"),
            "(from {}{}{}){} {}",
            client_info.current_address.toString(),
            (current_user != "default" ? ", user: " + current_user : ""),
            (!initial_query_id.empty() && current_query_id != initial_query_id ? ", initial_query_id: " + initial_query_id : std::string()),
            comment,
            joinLines(query));

        if (client_info.client_trace_context.trace_id != UUID())
        {
            LOG_TRACE(
                &Poco::Logger::get("executeQuery"),
                "OpenTelemetry traceparent '{}'",
                client_info.client_trace_context.composeTraceparentHeader());
        }
    }
}


/// Call this inside catch block.
static void setExceptionStackTrace(QueryLogElement & elem)
{
    /// Disable memory tracker for stack trace.
    /// Because if exception is "Memory limit (for query) exceed", then we probably can't allocate another one string.
    MemoryTracker::BlockerInThread temporarily_disable_memory_tracker(VariableContext::Global);

    try
    {
        throw;
    }
    catch (const std::exception & e)
    {
        elem.stack_trace = getExceptionStackTraceString(e);
    }
    catch (...)
    {
    }
}


/// Log exception (with query info) into text log (not into system table).
static void logException(ContextPtr context, QueryLogElement & elem)
{
    String comment;
    if (!elem.log_comment.empty())
        comment = fmt::format(" (comment: {})", elem.log_comment);

    if (elem.stack_trace.empty())
        LOG_ERROR(
            &Poco::Logger::get("executeQuery"),
            "{} (from {}){} (in query: {})",
            elem.exception,
            context->getClientInfo().current_address.toString(),
            comment,
            joinLines(elem.query));
    else
        LOG_ERROR(
            &Poco::Logger::get("executeQuery"),
            "{} (from {}){} (in query: {})"
            ", Stack trace (when copying this message, always include the lines below):\n\n{}",
            elem.exception,
            context->getClientInfo().current_address.toString(),
            comment,
            joinLines(elem.query),
            elem.stack_trace);
}

inline UInt64 time_in_microseconds(std::chrono::time_point<std::chrono::system_clock> timepoint)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(timepoint.time_since_epoch()).count();
}


inline UInt64 time_in_seconds(std::chrono::time_point<std::chrono::system_clock> timepoint)
{
    return std::chrono::duration_cast<std::chrono::seconds>(timepoint.time_since_epoch()).count();
}

static void onExceptionBeforeStart(const String & query_for_logging, ContextMutablePtr context, UInt64 current_time_us, ASTPtr ast)
{
    /// Exception before the query execution.
    if (auto quota = context->getQuota())
        quota->used(Quota::ERRORS, 1, /* check_exceeded = */ false);

    const Settings & settings = context->getSettingsRef();

    /// Log the start of query execution into the table if necessary.
    QueryLogElement elem;

    elem.type = QueryLogElementType::EXCEPTION_BEFORE_START;

    // all callers to onExceptionBeforeStart method construct the timespec for event_time and
    // event_time_microseconds from the same time point. So, it can be assumed that both of these
    // times are equal up to the precision of a second.
    elem.event_time = current_time_us / 1000000;
    elem.event_time_microseconds = current_time_us;
    elem.query_start_time = current_time_us / 1000000;
    elem.query_start_time_microseconds = current_time_us;

    elem.current_database = context->getCurrentDatabase();
    elem.query = query_for_logging;
    elem.normalized_query_hash = normalizedQueryHash<false>(query_for_logging);

    // We don't calculate query_kind, databases, tables and columns when the query isn't able to start

    elem.exception_code = getCurrentExceptionCode();
    elem.exception = getCurrentExceptionMessage(false);

    elem.client_info = context->getClientInfo();
    elem.partition_ids = context->getPartitionIds();

    elem.log_comment = settings.log_comment;
    if (elem.log_comment.size() > settings.max_query_size)
        elem.log_comment.resize(settings.max_query_size);

    if (settings.calculate_text_stack_trace)
        setExceptionStackTrace(elem);
    logException(context, elem);

    /// Update performance counters before logging to query_log
    CurrentThread::finalizePerformanceCounters();

    if (settings.log_queries && elem.type >= settings.log_queries_min_type
        && !settings.log_queries_min_query_duration_ms.totalMilliseconds())
        if (auto query_log = context->getQueryLog())
            query_log->add(elem);

    if (settings.enable_query_level_profiling)
    {
        insertCnchQueryMetric(
            context,
            query_for_logging,
            QueryLogElementType::EXCEPTION_BEFORE_START,
            current_time_us / 1000000,
            nullptr /*ast*/,
            nullptr /*query status info*/,
            nullptr /*stream info*/,
            nullptr /*query pipeline*/,
            false,
            0,
            0,
            0,
            elem.exception,
            elem.stack_trace);
    }

    if (auto opentelemetry_span_log = context->getOpenTelemetrySpanLog();
        context->query_trace_context.trace_id != UUID() && opentelemetry_span_log)
    {
        OpenTelemetrySpanLogElement span;
        span.trace_id = context->query_trace_context.trace_id;
        span.span_id = context->query_trace_context.span_id;
        span.parent_span_id = context->getClientInfo().client_trace_context.span_id;
        span.operation_name = "query";
        span.start_time_us = current_time_us;
        span.finish_time_us = current_time_us;

        /// Keep values synchronized to type enum in QueryLogElement::createBlock.
        span.attribute_names.push_back("clickhouse.query_status");
        span.attribute_values.push_back("ExceptionBeforeStart");

        span.attribute_names.push_back("db.statement");
        span.attribute_values.push_back(elem.query);

        span.attribute_names.push_back("clickhouse.query_id");
        span.attribute_values.push_back(elem.client_info.current_query_id);

        if (!context->query_trace_context.tracestate.empty())
        {
            span.attribute_names.push_back("clickhouse.tracestate");
            span.attribute_values.push_back(context->query_trace_context.tracestate);
        }

        opentelemetry_span_log->add(span);
    }

    ProfileEvents::increment(ProfileEvents::FailedQuery);

    if (ast)
    {
        if (ast->as<ASTSelectQuery>() || ast->as<ASTSelectWithUnionQuery>())
        {
            ProfileEvents::increment(ProfileEvents::FailedSelectQuery);
        }
        else if (ast->as<ASTInsertQuery>())
        {
            ProfileEvents::increment(ProfileEvents::FailedInsertQuery);
        }
    }
}

static void doSomeReplacementForSettings(ContextMutablePtr context)
{
    const Settings & settings = context->getSettingsRef();
    if (settings.enable_distributed_stages)
    {
        context->setSetting("enable_optimizer", Field(1));
        context->setSetting("enable_distributed_stages", Field(0));
    }
}

static void setQuerySpecificSettings(ASTPtr & ast, ContextMutablePtr context)
{
    if (auto * ast_insert_into = dynamic_cast<ASTInsertQuery *>(ast.get()))
    {
        if (ast_insert_into->watch)
            context->setSetting("output_format_enable_streaming", 1);
    }
}

static TransactionCnchPtr prepareCnchTransaction(ContextMutablePtr context, [[maybe_unused]] ASTPtr & ast)
{
    auto server_type = context->getServerType();

    if (server_type != ServerType::cnch_server && server_type != ServerType::cnch_worker)
        return {};
    if (auto txn = context->getCurrentTransaction(); txn)
    {
        LOG_DEBUG(&Poco::Logger::get("executeQuery"), "Cnch query is already in a transaction " + txn->getTransactionRecord().toString());
        return txn;
    }

    if (server_type == ServerType::cnch_server)
    {
        bool read_only = isReadOnlyTransaction(ast.get());
        auto session_txn = isQueryInInteractiveSession(context, ast)
            ? context->getSessionContext()->getCurrentTransaction()->as<CnchExplicitTransaction>()
            : nullptr;
        TxnTimestamp primary_txn_id = session_txn ? session_txn->getTransactionID() : TxnTimestamp{0};
        auto txn = context->getCnchTransactionCoordinator().createTransaction(
            CreateTransactionOption()
                .setContext(context)
                .setReadOnly(read_only)
                .setForceCleanByDM(context->getSettingsRef().force_clean_transaction_by_dm)
                .setAsyncPostCommit(context->getSettingsRef().async_post_commit)
                .setPrimaryTransactionId(primary_txn_id));
        context->setCurrentTransaction(txn);
        if (session_txn && !read_only)
            session_txn->addStatement(queryToString(ast));
        return txn;
    }
    else if (server_type == ServerType::cnch_worker)
    {
        /// TODO: test it
        bool is_initial_query = (context->getClientInfo().query_kind == ClientInfo::QueryKind::INITIAL_QUERY);

        String database;
        String table;
        if (auto * insert = ast->as<ASTInsertQuery>())
        {
            database = insert->table_id.database_name;
            table = insert->table_id.table_name;
        }
        else if (auto * system = ast->as<ASTSystemQuery>(); system && system->type == ASTSystemQuery::Type::DEDUP)
        {
            database = system->database;
            table = system->table;
        }

        if (is_initial_query && !table.empty())
        {
            if (database.empty())
                database = context->getCurrentDatabase();

            auto storage = DatabaseCatalog::instance().getTable(StorageID(database, table), context);
            if (!dynamic_cast<StorageCnchMergeTree *>(storage.get()) && !dynamic_cast<StorageCloudMergeTree *>(storage.get()))
                return {};

            auto host_ports = context->getCnchTopologyMaster()->getTargetServer(
                UUIDHelpers::UUIDToString(storage->getStorageUUID()), storage->getServerVwName(), true);
            auto server_client
                = host_ports.empty() ? context->getCnchServerClientPool().get() : context->getCnchServerClientPool().get(host_ports);
            auto txn = std::make_shared<CnchWorkerTransaction>(context->getGlobalContext(), server_client);
            context->setCurrentTransaction(txn);
            return txn;
        }
    }

    return {};
}

void interpretSettings(ASTPtr ast, ContextMutablePtr context)
{
    if (const auto * select_query = ast->as<ASTSelectQuery>())
    {
        if (auto new_settings = select_query->settings())
            InterpreterSetQuery(new_settings, context).executeForCurrentContext();
    }
    else if (const auto * select_with_union_query = ast->as<ASTSelectWithUnionQuery>())
    {
        if (!select_with_union_query->list_of_selects->children.empty())
        {
            // We might have an arbitrarily complex UNION tree, so just give
            // up if the last first-order child is not a plain SELECT.
            // It is flattened later, when we process UNION ALL/DISTINCT.
            const auto * last_select = select_with_union_query->list_of_selects->children.back()->as<ASTSelectQuery>();
            if (last_select && last_select->settings())
            {
                InterpreterSetQuery(last_select->settings(), context).executeForCurrentContext();
            }
        }
    }
    else if (const auto * query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get()))
    {
        if (query_with_output->settings_ast)
            InterpreterSetQuery(query_with_output->settings_ast, context).executeForCurrentContext();
    }
    else if (const auto * insert_query = ast->as<ASTInsertQuery>())
    {
        if (insert_query->settings_ast)
            InterpreterSetQuery(insert_query->settings_ast, context).executeForCurrentContext();
    }
}

static std::tuple<ASTPtr, BlockIO> executeQueryImpl(
    const char * begin,
    const char * end,
    ASTPtr input_ast,
    ContextMutablePtr context,
    bool internal,
    QueryProcessingStage::Enum stage,
    bool has_query_tail,
    ReadBuffer * istr)
{
    const auto current_time = std::chrono::system_clock::now();
    context->setQueryContext(context);

    auto & client_info = context->getClientInfo();

    // If it's not an internal query and we don't see an initial_query_start_time yet, initialize it
    // to current time. Internal queries are those executed without an independent client context,
    // thus should not set initial_query_start_time, because it might introduce data race. It's also
    // possible to have unset initial_query_start_time for non-internal and non-initial queries. For
    // example, the query is from an initiator that is running an old version of clickhouse.
    if (!internal && client_info.initial_query_start_time == 0)
    {
        client_info.initial_query_start_time = time_in_seconds(current_time);
        client_info.initial_query_start_time_microseconds = time_in_microseconds(current_time);
    }

#if !defined(ARCADIA_BUILD)
    assert(internal || CurrentThread::get().getQueryContext());
    assert(internal || CurrentThread::get().getQueryContext()->getCurrentQueryId() == CurrentThread::getQueryId());
#endif


    const Settings & settings = context->getSettingsRef();

    /// FIXME: Use global join for cnch join works for sql mode first.
    /// Will be replaced by distributed query after @youzhiyuan add query plan runtime.
    if (context->getServerType() == ServerType::cnch_server)
    {
        context->setSetting("distributed_product_mode", String{"global"});
    }

    ASTPtr ast;
    const char * query_end;

    /// Don't limit the size of internal queries.
    size_t max_query_size = 0;
    if (!internal)
        max_query_size = settings.max_query_size;

    auto finish_current_transaction = [ast](const ContextPtr & query_context) {
        if (auto cur_txn = query_context->getCurrentTransaction(); cur_txn)
        {
            if (query_context->getServerType() == ServerType::cnch_server)
            {
                query_context->getCnchTransactionCoordinator().finishTransaction(cur_txn);
            }
        }
    };
    String query_database;
    String query_table;
    BlockIO res;

    try
    {
        if (input_ast == nullptr)
        {
            ParserQuery parser(end, ParserSettings::valueOf(context->getSettings()));
            parser.setContext(context.get());

            /// TODO: parser should fail early when max_query_size limit is reached.
            ast = parseQuery(parser, begin, end, "", max_query_size, context->getSettings().max_parser_depth);
        }
        else
        {
            ast = input_ast;
        }
        if (context->getServerType() == ServerType::cnch_server && context->getVWCustomizedSettings())
        {
            auto vw_name = tryGetVirtualWarehouseName(ast, context);
            if (vw_name != EMPTY_VIRTUAL_WAREHOUSE_NAME)
            {
                context->getVWCustomizedSettings()->overwriteDefaultSettings(vw_name, context->getSettingsRef());
            }
        }

        if (isQueryInInteractiveSession(context, ast) && isDDLQuery(context, ast))
        {
            /// Commit the current explicit transaction
            LOG_WARNING(&Poco::Logger::get("executeQuery"), "Receive DDL in interactive transaction session, will commit the session implicitly");
            InterpreterCommitQuery(nullptr, context).execute();
        }

        if (context->getServerType() == ServerType::cnch_server
            && (isQueryInInteractiveSession(context, ast) || context->getSettingsRef().enable_auto_query_forwarding || settings.use_query_cache))
        {
            auto host_ports = getTargetServer(context, ast);
            LOG_DEBUG(
                &Poco::Logger::get("executeQuery"),
                "target server is {} and local rpc port is {}",
                host_ports.getRPCAddress(),
                context->getRPCPort());
            if (!host_ports.empty() && !isLocalServer(host_ports.getRPCAddress(), std::to_string(context->getRPCPort())))
            {
                LOG_DEBUG(
                    &Poco::Logger::get("executeQuery"), "Will reroute query " + queryToString(ast) + " to " + host_ports.getTCPAddress());
                context->initializeExternalTablesIfSet();
                executeQueryByProxy(context, host_ports, ast, res);
                LOG_DEBUG(&Poco::Logger::get("executeQuery"), "Query execution on remote server done");
                return std::make_tuple(ast, std::move(res));
            }
        }

        /// Interpret SETTINGS clauses as early as possible (before invoking the corresponding interpreter),
        /// to allow settings to take effect.
        if (input_ast == nullptr)
            interpretSettings(ast, context);

        if (const auto * query_with_table_output = dynamic_cast<const ASTQueryWithTableAndOutput *>(ast.get()))
        {
            query_database = query_with_table_output->database;
            query_table = query_with_table_output->table;
        }

        auto * insert_query = ast->as<ASTInsertQuery>();

        if (insert_query && insert_query->data)
        {
            query_end = insert_query->data;
            insert_query->has_tail = has_query_tail;
        }
        else
        {
            query_end = end;
        }
    }
    catch (...)
    {
        finish_current_transaction(context);
        /// Anyway log the query.
        String query = String(begin, begin + std::min(end - begin, static_cast<ptrdiff_t>(max_query_size)));

        auto query_for_logging = prepareQueryForLogging(query, context);
        logQuery(query_for_logging, context, internal);

        if (!internal)
        {
            onExceptionBeforeStart(query_for_logging, context, time_in_microseconds(current_time), ast);
        }

        throw;
    }

    doSomeReplacementForSettings(context);

    setQuerySpecificSettings(ast, context);

    bool can_use_query_cache = settings.use_query_cache && !internal && !ast->as<ASTExplainQuery>();

    auto txn = prepareCnchTransaction(context, ast);
    if (txn)
    {
        trySetVirtualWarehouseAndWorkerGroup(ast, context);
        if (context->getServerType() == ServerType::cnch_server)
        {
            context->initCnchServerResource(txn->getTransactionID());
            if (!internal && !ast->as<ASTShowProcesslistQuery>() && context->getSettingsRef().enable_query_queue)
                tryQueueQuery(context, ast->getType());
        }
    }

    /// Copy query into string. It will be written to log and presented in processlist. If an INSERT query, string will not include data to insertion.
    String query(begin, query_end);

    String query_for_logging;

    try
    {
        /// Replace ASTQueryParameter with ASTLiteral for prepared statements.
        if (context->hasQueryParameters())
        {
            ReplaceQueryParameterVisitor visitor(context->getQueryParameters());
            visitor.visit(ast);
            query = serializeAST(*ast);
        }

        /// MUST goes before any modification (except for prepared statements,
        /// since it substitute parameters and w/o them query does not contains
        /// parameters), to keep query as-is in query_log and server log.
        query_for_logging = prepareQueryForLogging(query, context);
        logQuery(query_for_logging, context, internal);

        /// Propagate WITH statement to children ASTSelect.
        if (settings.enable_global_with_statement)
        {
            ApplyWithGlobalVisitor().visit(ast);
        }

        {
            SelectIntersectExceptQueryVisitor::Data data{settings.intersect_default_mode, settings.except_default_mode};
            SelectIntersectExceptQueryVisitor{data}.visit(ast);
        }

        {
            /// Normalize SelectWithUnionQuery
            NormalizeSelectWithUnionQueryVisitor::Data data{settings.union_default_mode};
            NormalizeSelectWithUnionQueryVisitor{data}.visit(ast);
        }

        /// Check the limits.
        checkASTSizeLimits(*ast, settings);

        /// Put query to process list. But don't put SHOW PROCESSLIST query itself.
        ProcessList::EntryPtr process_list_entry;
        if (!internal && !ast->as<ASTShowProcesslistQuery>())
        {
            /// processlist also has query masked now, to avoid secrets leaks though SHOW PROCESSLIST by other users.
            process_list_entry = context->getProcessList().insert(query_for_logging, ast.get(), context);
            context->setProcessListEntry(process_list_entry);
        }

        /// Calculate the time duration of building query pipeline, start right after creating processing list to make it consistent with the calcuation of query latency.
        Stopwatch watch;

        /// Load external tables if they were provided
        context->initializeExternalTablesIfSet();

        // disable optimizer for internal query
        if (internal)
            context->setSetting("enable_optimizer", Field(0));

        auto * insert_query = ast->as<ASTInsertQuery>();
        if (insert_query && insert_query->select)
        {
            /// Prepare Input storage before executing interpreter if we already got a buffer with data.
            if (istr)
            {
                ASTPtr input_function;
                insert_query->tryFindInputFunction(input_function);
                if (input_function)
                {
                    StoragePtr storage = context->executeTableFunction(input_function);
                    auto & input_storage = dynamic_cast<StorageInput &>(*storage);
                    auto input_metadata_snapshot = input_storage.getInMemoryMetadataPtr();
                    auto pipe
                        = getSourceFromFromASTInsertQuery(ast, istr, input_metadata_snapshot->getSampleBlock(), context, input_function);
                    input_storage.setPipe(std::move(pipe));
                }
            }
        }
        else
            /// reset Input callbacks if query is not INSERT SELECT
            context->resetInputCallbacks();

        context->markReadFromClientFinished();

        auto interpreter = InterpreterFactory::get(ast, context, SelectQueryOptions(stage).setInternal(internal));

        std::shared_ptr<const EnabledQuota> quota;
        if (!interpreter->ignoreQuota())
        {
            quota = context->getQuota();
            if (quota)
            {
                if (ast->as<ASTSelectQuery>() || ast->as<ASTSelectWithUnionQuery>())
                {
                    quota->used(Quota::QUERY_SELECTS, 1);
                }
                else if (ast->as<ASTInsertQuery>())
                {
                    quota->used(Quota::QUERY_INSERTS, 1);
                }
                quota->used(Quota::QUERIES, 1);
                quota->checkExceeded(Quota::ERRORS);
            }
        }

        StreamLocalLimits limits;
        if (!interpreter->ignoreLimits())
        {
            limits.mode = LimitsMode::LIMITS_CURRENT; //-V1048
            limits.size_limits = SizeLimits(settings.max_result_rows, settings.max_result_bytes, settings.result_overflow_mode);
        }


        bool read_result_from_query_cache = false; /// a query must not read from *and* write to the query cache at the same time
        TxnTimestamp source_update_time_for_query_cache = TxnTimestamp::minTS();
        {
            OpenTelemetrySpanHolder span("IInterpreter::execute()");
            try
            {
                res = interpreter->execute();
            }
            catch (...)
            {
                if (typeid_cast<const InterpreterSelectQueryUseOptimizer *>(&*interpreter))
                {
                    // fallback to simple query process
                    if (context->getSettingsRef().enable_optimizer_fallback)
                    {
                        LOG_INFO(&Poco::Logger::get("executeQuery"), "Query failed in optimizer enabled, try to fallback to simple query.");
                        turnOffOptimizer(context, ast);
                        auto retry_interpreter = InterpreterFactory::get(ast, context, stage);
                        res = retry_interpreter->execute();

                        // Used to identify 'fallback' queries in query_log
                        context->setSetting("operator_profile_receive_timeout", 3001);
                    }
                    else
                    {
                        LOG_INFO(&Poco::Logger::get("executeQuery"), "Query failed in optimizer enabled, throw exception.");
                        throw;
                    }
                }
                else if (
                    !context->getSettingsRef().enable_optimizer && context->getSettingsRef().distributed_perfect_shard
                    && context->getSettingsRef().fallback_perfect_shard)
                {
                    LOG_INFO(&Poco::Logger::get("executeQuery"), "Query failed in perfect-shard enabled, try to fallback to normal mode.");
                    InterpreterPerfectShard::turnOffPerfectShard(context, ast);
                    auto retry_interpreter = InterpreterFactory::get(ast, context, stage);
                    res = retry_interpreter->execute();
                }
                else
                {
                    throw;
                }
            }

            const std::set<StorageID> storage_ids = res.pipeline.getUsedStorageIDs();
            LOG_DEBUG(&Poco::Logger::get("executeQuery"), "pipeline has all used StorageIDs: {}", res.pipeline.hasAllUsedStorageIDs());
            auto query_cache = context->getQueryCache();
            if (query_cache != nullptr
                && (can_use_query_cache && settings.enable_reads_from_query_cache)
                && (res.pipeline.getNumStreams() > 0) && (res.pipeline.hasAllUsedStorageIDs()) && (!storage_ids.empty()))
            {
                LOG_DEBUG(&Poco::Logger::get("executeQuery"), "StorageIDs:");
                for (auto & storage_id : storage_ids)
                    LOG_DEBUG(&Poco::Logger::get("executeQuery"), "StorageID {}", storage_id.getNameForLogs());
                if (settings.enable_transactional_query_cache)
                    source_update_time_for_query_cache = getMaxUpdateTime(storage_ids, context);
                LOG_DEBUG(&Poco::Logger::get("executeQuery"), "max update timestamp {}", source_update_time_for_query_cache);
                if (source_update_time_for_query_cache.toUInt64() != 0)
                {
                    QueryCache::Key key(
                        ast,
                        res.pipeline.getHeader(),
                        context->getUserName(),
                        /*dummy for is_shared*/ false,
                        /*dummy value for expires_at*/ std::chrono::system_clock::from_time_t(1),
                        /*dummy value for is_compressed*/ false,
                        context->getCurrentTransactionID());
                    QueryCache::Reader reader = query_cache->createReader(key, source_update_time_for_query_cache);
                    if (reader.hasCacheEntryForKey())
                    {
                        QueryPipeline pipeline;
                        pipeline.readFromQueryCache(reader.getSource(), reader.getSourceTotals(), reader.getSourceExtremes());
                        res.pipeline = std::move(pipeline);
                        read_result_from_query_cache = true;
                    }
                }
            }
        }

        QueryPipeline & pipeline = res.pipeline;
        bool use_processors = pipeline.initialized();

        if (const auto * insert_interpreter = typeid_cast<const InterpreterInsertQuery *>(&*interpreter))
        {
            /// Save insertion table (not table function). TODO: support remote() table function.
            auto table_id = insert_interpreter->getDatabaseTable();
            if (!table_id.empty())
                context->setInsertionTable(std::move(table_id));
        }

        /// FIXME: Fix after complex query is supported
        bool complex_query = false;
        UInt32 init_time = watch.elapsedMilliseconds();

        if (process_list_entry)
        {
            /// Query was killed before execution
            if ((*process_list_entry)->isKilled())
                throw Exception(
                    "Query '" + (*process_list_entry)->getInfo().client_info.current_query_id + "' is killed in pending state",
                    ErrorCodes::QUERY_WAS_CANCELLED);
            else if (!use_processors)
                (*process_list_entry)->setQueryStreams(res);
        }

        /// Hold element of process list till end of query execution.
        res.process_list_entry = process_list_entry;

        if (use_processors)
        {
            /// Limits on the result, the quota on the result, and also callback for progress.
            /// Limits apply only to the final result.
            pipeline.setProgressCallback(context->getProgressCallback());
            pipeline.setProcessListElement(context->getProcessListElement());
            if (stage == QueryProcessingStage::Complete && !pipeline.isCompleted())
            {
                pipeline.resize(1);
                pipeline.addSimpleTransform([&](const Block & header) {
                    auto transform = std::make_shared<LimitsCheckingTransform>(header, limits);
                    transform->setQuota(quota);
                    return transform;
                });
            }
        }
        else
        {
            /// Limits on the result, the quota on the result, and also callback for progress.
            /// Limits apply only to the final result.
            if (res.in)
            {
                res.in->setProgressCallback(context->getProgressCallback());
                res.in->setProcessListElement(context->getProcessListElement());
                if (stage == QueryProcessingStage::Complete)
                {
                    if (!interpreter->ignoreQuota())
                        res.in->setQuota(quota);
                    if (!interpreter->ignoreLimits())
                        res.in->setLimits(limits);
                }
            }

            if (res.out)
            {
                if (auto * stream = dynamic_cast<CountingBlockOutputStream *>(res.out.get()))
                {
                    stream->setProcessListElement(context->getProcessListElement());
                }
            }
        }

        {
            /// If
            /// - it is a SELECT query, and
            /// - active (write) use of the query cache is enabled
            /// then add a processor on top of the pipeline which stores the result in the query cache.

            auto query_cache = context->getQueryCache();
            if (!read_result_from_query_cache
                && query_cache != nullptr
                && can_use_query_cache && settings.enable_writes_to_query_cache
                && (res.pipeline.getNumStreams() > 0)
                && (!astContainsNonDeterministicFunctions(ast, context) || settings.query_cache_store_results_of_queries_with_nondeterministic_functions))
            {
                QueryCache::Key key(
                    ast, res.pipeline.getHeader(),
                    context->getUserName(), settings.query_cache_share_between_users,
                    std::chrono::system_clock::now() + std::chrono::seconds(settings.query_cache_ttl),
                    settings.query_cache_compress_entries,
                    context->getCurrentTransactionID());

                const size_t num_query_runs = query_cache->recordQueryRun(key);
                if (num_query_runs > settings.query_cache_min_query_runs)
                {
                    auto query_cache_writer = std::make_shared<QueryCache::Writer>(query_cache->createWriter(
                                     key,
                                     std::chrono::milliseconds(settings.query_cache_min_query_duration.totalMilliseconds()),
                                     settings.query_cache_squash_partial_results,
                                     settings.max_block_size,
                                     settings.query_cache_max_size_in_bytes,
                                     settings.query_cache_max_entries,
                                     source_update_time_for_query_cache));
                    res.pipeline.writeResultIntoQueryCache(query_cache_writer);
                }
            }
        }

        /// Everything related to query log.
        {
            QueryLogElement elem;

            elem.type = QueryLogElementType::QUERY_START; //-V1048

            elem.event_time = time_in_seconds(current_time);
            elem.event_time_microseconds = time_in_microseconds(current_time);
            elem.query_start_time = time_in_seconds(current_time);
            elem.query_start_time_microseconds = time_in_microseconds(current_time);

            elem.current_database = context->getCurrentDatabase();
            elem.query = query_for_logging;
            elem.normalized_query_hash = normalizedQueryHash<false>(query_for_logging);

            elem.client_info = client_info;
            elem.partition_ids = context->getPartitionIds();

            if (!context->getSettingsRef().enable_optimizer)
            {
                elem.segment_id = -1;
                elem.segment_parallel = -1;
                elem.segment_parallel_index = -1;
            }
            else
            {
                elem.segment_id = 0;
                elem.segment_parallel = 1;
                elem.segment_parallel_index = 1;
            }

            bool log_queries = settings.log_queries && !internal;

            /// Log into system table start of query execution, if need.
            if (log_queries)
            {
                if (use_processors)
                {
                    const auto & info = context->getQueryAccessInfo();
                    elem.query_databases = info.databases;
                    elem.query_tables = info.tables;
                    elem.query_columns = info.columns;
                    elem.query_projections = info.projections;
                    /// Optimizer match materialized views
                    if (context->getOptimizerMetrics())
                    {
                        for (const auto & view_id : context->getOptimizerMetrics()->getUsedMaterializedViews())
                        {
                            elem.query_materialized_views.emplace(view_id.getFullNameNotQuoted());
                        }
                    }
                }

                interpreter->extendQueryLogElem(elem, ast, context, query_database, query_table);

                if (settings.log_query_settings)
                    elem.query_settings = std::make_shared<Settings>(context->getSettingsRef());

                elem.log_comment = settings.log_comment;
                if (elem.log_comment.size() > settings.max_query_size)
                    elem.log_comment.resize(settings.max_query_size);

                if (elem.type >= settings.log_queries_min_type && !settings.log_queries_min_query_duration_ms.totalMilliseconds())
                {
                    if (auto query_log = context->getQueryLog())
                        query_log->add(elem);
                }
            }

            if (settings.enable_query_level_profiling && context->getServerType() == ServerType::cnch_server)
            {
                /// Only query_metrics will include the `query_start` records, query_worker_metrics will not.
                insertCnchQueryMetric(
                    context,
                    query,
                    QueryMetricLogState::QUERY_START,
                    time_in_seconds(current_time),
                    ast,
                    nullptr,
                    nullptr,
                    nullptr,
                    false,
                    complex_query,
                    init_time,
                    0,
                    "",
                    "");
            }

            /// Common code for finish and exception callbacks
            auto status_info_to_query_log = [](QueryLogElement & element, const QueryStatusInfo & info, const ASTPtr query_ast) mutable {
                DB::UInt64 query_time = info.elapsed_seconds * 1000000;
                ProfileEvents::increment(ProfileEvents::QueryTimeMicroseconds, query_time);
                if (query_ast->as<ASTSelectQuery>() || query_ast->as<ASTSelectWithUnionQuery>())
                {
                    ProfileEvents::increment(ProfileEvents::SelectQueryTimeMicroseconds, query_time);
                }
                else if (query_ast->as<ASTInsertQuery>())
                {
                    ProfileEvents::increment(ProfileEvents::InsertQueryTimeMicroseconds, query_time);
                }

                element.query_duration_ms = info.elapsed_seconds * 1000;

                element.read_rows = info.read_rows;
                element.read_bytes = info.read_bytes;

                element.written_rows = info.written_rows;
                element.written_bytes = info.written_bytes;

                element.memory_usage = info.peak_memory_usage > 0 ? info.peak_memory_usage : 0;

                element.thread_ids = std::move(info.thread_ids);
                element.profile_counters = std::move(info.profile_counters);

                element.max_io_time_thread_name = std::move(info.max_io_time_thread_name);
                element.max_io_time_thread_ms = info.max_io_time_thread_ms;
                element.max_thread_io_profile_counters = std::move(info.max_io_thread_profile_counters);
            };

            auto query_id = context->getCurrentQueryId();
            /// Also make possible for caller to log successful query finish and exception during execution.
            auto finish_callback
                =
                    [elem,
                     context,
                     query,
                     ast,
                     my_can_use_query_cache = can_use_query_cache,
                     enable_writes_to_query_cache = settings.enable_writes_to_query_cache,
                     query_cache_store_results_of_queries_with_nondeterministic_functions = settings.query_cache_store_results_of_queries_with_nondeterministic_functions,
                     log_queries,
                     log_queries_min_type = settings.log_queries_min_type,
                     log_queries_min_query_duration_ms = settings.log_queries_min_query_duration_ms.totalMilliseconds(),
                     log_processors_profiles = settings.log_processors_profiles,
                     status_info_to_query_log,
                     query_id,
                     finish_current_transaction,
                     complex_query,
                     pulling_pipeline = (res.pipeline.getNumStreams() > 0),
                     init_time](
                        IBlockInputStream * stream_in,
                        IBlockOutputStream * stream_out,
                        QueryPipeline * query_pipeline,
                        UInt64 runtime_latency) mutable {
                        /// If active (write) use of the query cache is enabled and the query is eligible for result caching, then store the query
                        /// result buffered in the special-purpose cache processor (added on top of the pipeline) into the cache.
                        auto query_cache = context->getQueryCache();
                        if (query_cache != nullptr
                            && pulling_pipeline
                            && my_can_use_query_cache && enable_writes_to_query_cache
                            && (!astContainsNonDeterministicFunctions(ast, context) || query_cache_store_results_of_queries_with_nondeterministic_functions))
                        {
                            query_pipeline->finalizeWriteInQueryCache();
                        }

                        finish_current_transaction(context);
                        QueryStatus * process_list_elem = context->getProcessListElement();

                        if (!process_list_elem)
                            return;

                        /// Update performance counters before logging to query_log
                        CurrentThread::finalizePerformanceCounters();

                        QueryStatusInfo info = process_list_elem->getInfo(true, context->getSettingsRef().log_profile_events);

                        double elapsed_seconds = info.elapsed_seconds;

                        elem.type = QueryLogElementType::QUERY_FINISH;

                        // construct event_time and event_time_microseconds using the same time point
                        // so that the two times will always be equal up to a precision of a second.
                        const auto finish_time = std::chrono::system_clock::now();
                        elem.event_time = time_in_seconds(finish_time);
                        elem.event_time_microseconds = time_in_microseconds(finish_time);
                        status_info_to_query_log(elem, info, ast);

                        auto progress_callback = context->getProgressCallback();

                        if (progress_callback)
                            progress_callback(Progress(WriteProgress(info.written_rows, info.written_bytes)));

                        if (stream_in)
                        {
                            const BlockStreamProfileInfo & stream_in_info = stream_in->getProfileInfo();

                            /// NOTE: INSERT SELECT query contains zero metrics
                            elem.result_rows = stream_in_info.rows;
                            elem.result_bytes = stream_in_info.bytes;
                        }
                        else if (stream_out) /// will be used only for ordinary INSERT queries
                        {
                            if (const auto * counting_stream = dynamic_cast<const CountingBlockOutputStream *>(stream_out))
                            {
                                /// NOTE: Redundancy. The same values could be extracted from process_list_elem->progress_out.query_settings = process_list_elem->progress_in
                                elem.result_rows = counting_stream->getProgress().read_rows;
                                elem.result_bytes = counting_stream->getProgress().read_bytes;
                            }
                        }
                        else if (query_pipeline)
                        {
                            if (const auto * output_format = query_pipeline->getOutputFormat())
                            {
                                elem.result_rows = output_format->getResultRows();
                                elem.result_bytes = output_format->getResultBytes();
                            }
                        }

                        if (elem.read_rows != 0)
                        {
                            LOG_INFO(
                                &Poco::Logger::get("executeQuery"),
                                "Read {} rows, {} in {} sec., {} rows/sec., {}/sec.",
                                elem.read_rows,
                                ReadableSize(elem.read_bytes),
                                elapsed_seconds,
                                static_cast<size_t>(elem.read_rows / elapsed_seconds),
                                ReadableSize(elem.read_bytes / elapsed_seconds));
                        }

                        if (context->getSettingsRef().enable_query_level_profiling)
                        {
                            if (stream_in)
                            {
                                insertCnchQueryMetric(
                                    context,
                                    query,
                                    QueryMetricLogState::QUERY_FINISH,
                                    time(nullptr),
                                    ast,
                                    &info,
                                    &stream_in->getProfileInfo(),
                                    nullptr,
                                    false,
                                    complex_query,
                                    init_time,
                                    runtime_latency,
                                    "",
                                    "");
                            }
                            else if (stream_out)
                            {
                                insertCnchQueryMetric(
                                    context,
                                    query,
                                    QueryMetricLogState::QUERY_FINISH,
                                    time(nullptr),
                                    ast,
                                    &info,
                                    nullptr,
                                    nullptr,
                                    false,
                                    complex_query,
                                    init_time,
                                    runtime_latency,
                                    "",
                                    "");
                            }
                            else if (query_pipeline)
                            {
                                insertCnchQueryMetric(
                                    context,
                                    query,
                                    QueryMetricLogState::QUERY_FINISH,
                                    time(nullptr),
                                    ast,
                                    &info,
                                    nullptr,
                                    query_pipeline,
                                    false,
                                    complex_query,
                                    init_time,
                                    runtime_latency,
                                    "",
                                    "");
                            }
                            else
                            {
                                insertCnchQueryMetric(
                                    context,
                                    query,
                                    QueryMetricLogState::QUERY_FINISH,
                                    time(nullptr),
                                    ast,
                                    &info,
                                    nullptr,
                                    nullptr,
                                    true,
                                    complex_query,
                                    init_time,
                                    runtime_latency,
                                    "",
                                    "");
                            }
                        }

                        elem.thread_ids = std::move(info.thread_ids);
                        elem.profile_counters = std::move(info.profile_counters);
                        elem.max_io_time_thread_name = std::move(info.max_io_time_thread_name);
                        elem.max_io_time_thread_ms = info.max_io_time_thread_ms;
                        elem.max_thread_io_profile_counters = std::move(info.max_io_thread_profile_counters);


                        const auto & factories_info = context->getQueryFactoriesInfo();
                        elem.used_aggregate_functions = factories_info.aggregate_functions;
                        elem.used_aggregate_function_combinators = factories_info.aggregate_function_combinators;
                        elem.used_database_engines = factories_info.database_engines;
                        elem.used_data_type_families = factories_info.data_type_families;
                        elem.used_dictionaries = factories_info.dictionaries;
                        elem.used_formats = factories_info.formats;
                        elem.used_functions = factories_info.functions;
                        elem.used_storages = factories_info.storages;
                        elem.used_table_functions = factories_info.table_functions;
                        elem.partition_ids = context->getPartitionIds();

                        if (log_queries && elem.type >= log_queries_min_type
                            && Int64(elem.query_duration_ms) >= log_queries_min_query_duration_ms)
                        {
                            if (auto query_log = context->getQueryLog())
                                query_log->add(elem);
                        }

                        if (log_processors_profiles)
                        {
                            auto processors_profile_log = context->getProcessorsProfileLog();
                            if (query_pipeline && processors_profile_log)
                                processors_profile_log->addLogs(query_pipeline, elem.client_info.current_query_id, finish_time);
                        }


                        if (auto opentelemetry_span_log = context->getOpenTelemetrySpanLog();
                            context->query_trace_context.trace_id != UUID() && opentelemetry_span_log)
                        {
                            OpenTelemetrySpanLogElement span;
                            span.trace_id = context->query_trace_context.trace_id;
                            span.span_id = context->query_trace_context.span_id;
                            span.parent_span_id = context->getClientInfo().client_trace_context.span_id;
                            span.operation_name = "query";
                            span.start_time_us = elem.query_start_time_microseconds;
                            span.finish_time_us = time_in_microseconds(finish_time);

                            /// Keep values synchronized to type enum in QueryLogElement::createBlock.
                            span.attribute_names.push_back("clickhouse.query_status");
                            span.attribute_values.push_back("QueryFinish");

                            span.attribute_names.push_back("db.statement");
                            span.attribute_values.push_back(elem.query);

                            span.attribute_names.push_back("clickhouse.query_id");
                            span.attribute_values.push_back(elem.client_info.current_query_id);
                            if (!context->query_trace_context.tracestate.empty())
                            {
                                span.attribute_names.push_back("clickhouse.tracestate");
                                span.attribute_values.push_back(context->query_trace_context.tracestate);
                            }

                            opentelemetry_span_log->add(span);
                        }

                        if (const String & async_query_id = context->getAsyncQueryId(); !async_query_id.empty())
                        {
                            updateAsyncQueryStatus(context, async_query_id, query_id, AsyncQueryStatus::Finished);
                        }

                        // cancel coordinator itself
                        context->getPlanSegmentProcessList().tryCancelPlanSegmentGroup(query_id);
                        SegmentSchedulerPtr scheduler = context->getSegmentScheduler();
                        scheduler->finishPlanSegments(query_id);
                        RuntimeFilterManager::getInstance().removeQuery(query_id);
                    };

            auto exception_callback = [elem,
                                       context,
                                       query,
                                       ast,
                                       log_queries,
                                       log_queries_min_type = settings.log_queries_min_type,
                                       log_queries_min_query_duration_ms = settings.log_queries_min_query_duration_ms.totalMilliseconds(),
                                       quota(quota),
                                       status_info_to_query_log,
                                       query_id,
                                       finish_current_transaction,
                                       complex_query,
                                       init_time](UInt64 runtime_latency) mutable {
                finish_current_transaction(context);
                if (quota)
                    quota->used(Quota::ERRORS, 1, /* check_exceeded = */ false);

                elem.type = QueryLogElementType::EXCEPTION_WHILE_PROCESSING;

                // event_time and event_time_microseconds are being constructed from the same time point
                // to ensure that both the times will be equal up to the precision of a second.
                const auto time_now = std::chrono::system_clock::now();

                elem.event_time = time_in_seconds(time_now);
                elem.event_time_microseconds = time_in_microseconds(time_now);
                elem.query_duration_ms = 1000 * (elem.event_time - elem.query_start_time);
                elem.exception_code = getCurrentExceptionCode();
                elem.exception = getCurrentExceptionMessage(false);
                elem.partition_ids = context->getPartitionIds();

                QueryStatus * process_list_elem = context->getProcessListElement();
                const Settings & current_settings = context->getSettingsRef();

                /// Update performance counters before logging to query_log
                CurrentThread::finalizePerformanceCounters();

                if (process_list_elem)
                {
                    QueryStatusInfo info = process_list_elem->getInfo(true, current_settings.log_profile_events, false);
                    status_info_to_query_log(elem, info, ast);
                }

                if (current_settings.calculate_text_stack_trace)
                    setExceptionStackTrace(elem);

                bool throw_root_cause = false;
                auto coordinator = MPPQueryManager::instance().getCoordinator(query_id);
                if (coordinator)
                {
                    coordinator->updateSegmentInstanceStatus(RuntimeSegmentsStatus{
                        .query_id = query_id,
                        .segment_id = 0,
                        .is_succeed = false,
                        .message = elem.exception,
                        .code = elem.exception_code});
                    if (isAmbiguosError(elem.exception_code))
                    {
                        auto query_status = coordinator->waitUntilFinish(elem.exception_code, elem.exception);
                        throw_root_cause = query_status.error_code != elem.exception_code;
                        elem.exception_code = query_status.error_code;
                        elem.exception = query_status.summarized_error_msg;
                    }
                }

                logException(context, elem);

                /// In case of exception we log internal queries also
                if (log_queries && elem.type >= log_queries_min_type && Int64(elem.query_duration_ms) >= log_queries_min_query_duration_ms)
                {
                    if (auto query_log = context->getQueryLog())
                        query_log->add(elem);
                }

                if (context->getSettingsRef().enable_query_level_profiling)
                {
                    QueryStatusInfo info = process_list_elem->getInfo(true, context->getSettingsRef().log_profile_events);
                    insertCnchQueryMetric(
                        context,
                        query,
                        QueryMetricLogState::EXCEPTION_WHILE_PROCESSING,
                        time(nullptr),
                        ast,
                        &info,
                        nullptr,
                        nullptr,
                        false,
                        complex_query,
                        init_time,
                        runtime_latency,
                        elem.exception,
                        elem.stack_trace);
                }

                ProfileEvents::increment(ProfileEvents::FailedQuery);
                if (ast->as<ASTSelectQuery>() || ast->as<ASTSelectWithUnionQuery>())
                {
                    ProfileEvents::increment(ProfileEvents::FailedSelectQuery);
                }
                else if (ast->as<ASTInsertQuery>())
                {
                    ProfileEvents::increment(ProfileEvents::FailedInsertQuery);
                }

                if (const String & async_query_id = context->getAsyncQueryId(); !async_query_id.empty())
                {
                    updateAsyncQueryStatus(context, async_query_id, query_id, AsyncQueryStatus::Failed, elem.exception);
                }

                auto coodinator = MPPQueryManager::instance().getCoordinator(query_id);
                if (coodinator)
                    coodinator->updateSegmentInstanceStatus(RuntimeSegmentsStatus{
                        .query_id = query_id,
                        .segment_id = 0,
                        .is_succeed = false,
                        .message = elem.exception,
                        .code = elem.exception_code});
                if (throw_root_cause)
                {
                    throw Exception(elem.exception, elem.exception_code);
                }
            };

            res.finish_callback = std::move(finish_callback);
            res.exception_callback = std::move(exception_callback);

            if (!internal && res.in)
            {
                WriteBufferFromOwnString msg_buf;
                res.in->dumpTree(msg_buf);
                LOG_DEBUG(&Poco::Logger::get("executeQuery"), "Query pipeline:\n{}", msg_buf.str());
            }
        }
    }
    catch (...)
    {
        finish_current_transaction(context);

        if (!internal)
        {
            if (query_for_logging.empty())
                query_for_logging = prepareQueryForLogging(query, context);

            onExceptionBeforeStart(query_for_logging, context, time_in_microseconds(current_time), ast);
        }

        throw;
    }

    return std::make_tuple(ast, std::move(res));
}


BlockIO
executeQuery(const String & query, ContextMutablePtr context, bool internal, QueryProcessingStage::Enum stage, bool may_have_embedded_data)
{
    ASTPtr ast;
    BlockIO streams;

    std::tie(ast, streams)
        = executeQueryImpl(query.data(), query.data() + query.size(), nullptr, context, internal, stage, !may_have_embedded_data, nullptr);

    if (const auto * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get()))
    {

        String format_name = ast_query_with_output->format ? getIdentifierName(ast_query_with_output->format) : context->getDefaultFormat();

        if (format_name == "Null")
            streams.null_format = true;
    }

    return streams;
}

BlockIO executeQuery(
    const String & query,
    ASTPtr ast,
    ContextMutablePtr context,
    bool internal,
    QueryProcessingStage::Enum stage,
    bool may_have_embedded_data)
{
    BlockIO streams;
    std::tie(ast, streams)
        = executeQueryImpl(query.data(), query.data() + query.size(), ast, context, internal, stage, !may_have_embedded_data, nullptr);

    if (const auto * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get()))
    {
        String format_name = ast_query_with_output->format ? getIdentifierName(ast_query_with_output->format) : context->getDefaultFormat();

        if (format_name == "Null")
            streams.null_format = true;
    }

    return streams;
}

BlockIO executeQuery(
    const String & query,
    ContextMutablePtr context,
    bool internal,
    QueryProcessingStage::Enum stage,
    bool may_have_embedded_data,
    bool allow_processors)
{
    BlockIO res = executeQuery(query, context, internal, stage, may_have_embedded_data);

    if (!allow_processors && res.pipeline.initialized())
        res.in = res.getInputStream();

    return res;
}


void executeQuery(
    ReadBuffer & istr,
    WriteBuffer & ostr,
    bool allow_into_outfile,
    ContextMutablePtr context,
    std::function<void(const String &, const String &, const String &, const String &)> set_result_details,
    const std::optional<FormatSettings> & output_format_settings,
    bool internal)
{
    PODArray<char> parse_buf;
    const char * begin;
    const char * end;

    /// If 'istr' is empty now, fetch next data into buffer.
    if (!istr.hasPendingData())
        istr.next();

    size_t max_query_size = context->getSettingsRef().max_query_size;

    bool may_have_tail;
    if (istr.buffer().end() - istr.position() > static_cast<ssize_t>(max_query_size))
    {
        /// If remaining buffer space in 'istr' is enough to parse query up to 'max_query_size' bytes, then parse inplace.
        begin = istr.position();
        end = istr.buffer().end();
        istr.position() += end - begin;
        /// Actually we don't know will query has additional data or not.
        /// But we can't check istr.eof(), because begin and end pointers will become invalid
        may_have_tail = true;
    }
    else
    {
        /// If not - copy enough data into 'parse_buf'.
        WriteBufferFromVector<PODArray<char>> out(parse_buf);
        LimitReadBuffer limit(istr, max_query_size + 1, false);
        copyData(limit, out);
        out.finalize();

        begin = parse_buf.data();
        end = begin + parse_buf.size();
        /// Can check stream for eof, because we have copied data
        may_have_tail = !istr.eof();
    }

    ASTPtr ast;
    BlockIO streams;

    ParserQuery parser(end, ParserSettings::valueOf(context->getSettings()));
    parser.setContext(context.get());

    /// TODO: parser should fail early when max_query_size limit is reached.
    ast = parseQuery(parser, begin, end, "", max_query_size, context->getSettings().max_parser_depth);
    interpretSettings(ast, context);

    auto * insert_query = ast->as<ASTInsertQuery>();

    if (!(insert_query && insert_query->data) && context->isAsyncMode())
    {
        String query(begin, end);
        executeHttpQueryInAsyncMode(query, ast, context, ostr, &istr, may_have_tail, output_format_settings, set_result_details);
        return;
    }
    else
    {
        std::tie(ast, streams) = executeQueryImpl(begin, end, ast, context, internal, QueryProcessingStage::Complete, may_have_tail, &istr);
    }

    auto & pipeline = streams.pipeline;

    std::exception_ptr exception;
    try
    {
        if (streams.out)
        {
            auto pipe = getSourceFromFromASTInsertQuery(ast, &istr, streams.out->getHeader(), context, nullptr);

            pipeline.init(std::move(pipe));
            pipeline.resize(1);
            pipeline.setSinks([&](const Block &, Pipe::StreamType) { return std::make_shared<SinkToOutputStream>(streams.out); });

            auto executor = pipeline.execute();
            executor->execute(pipeline.getNumThreads());
        }
        else if (streams.in)
        {
            const auto * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get());

            WriteBuffer * out_buf = &ostr;
            std::optional<String> out_path;
            std::optional<WriteBufferFromFile> out_file_buf;
#if USE_HDFS
            std::unique_ptr<WriteBufferFromHDFS> out_hdfs_raw;
            std::optional<ZlibDeflatingWriteBuffer> out_hdfs_buf;
#endif
            if (ast_query_with_output && ast_query_with_output->out_file)
            {
                out_path.emplace(typeid_cast<const ASTLiteral &>(*ast_query_with_output->out_file).value.safeGet<std::string>());
                const Poco::URI out_uri(*out_path);
                const String & scheme = out_uri.getScheme();

                if (scheme.empty())
                {
                    if (!allow_into_outfile)
                        throw Exception("INTO OUTFILE is not allowed", ErrorCodes::INTO_OUTFILE_NOT_ALLOWED);

                    out_file_buf.emplace(*out_path, DBMS_DEFAULT_BUFFER_SIZE, O_WRONLY | O_EXCL | O_CREAT);
                    out_buf = &*out_file_buf;
                }
#    if USE_HDFS
                else if (DB::isHdfsOrCfsScheme(scheme))
                {
                    out_hdfs_raw = std::make_unique<WriteBufferFromHDFS>(
                        *out_path, context->getHdfsConnectionParams(), context->getSettingsRef().max_hdfs_write_buffer_size);
                    int compression_level = Z_DEFAULT_COMPRESSION;
                    out_hdfs_buf.emplace(std::move(out_hdfs_raw), CompressionMethod::Gzip, compression_level);
                    out_buf = &*out_hdfs_buf;
                }
#    endif
                else
                {
                    throw Exception(
                        "Path: " + *out_path + " is illegal, only support write query result to local file or tos",
                        ErrorCodes::CANNOT_PARSE_DOMAIN_VALUE_FROM_STRING);
                }
            }

            String format_name = ast_query_with_output && (ast_query_with_output->format != nullptr)
                ? getIdentifierName(ast_query_with_output->format)
                : context->getDefaultFormat();

            auto out = FormatFactory::instance().getOutputStreamParallelIfPossible(
                format_name, *out_buf, streams.in->getHeader(), context, {}, output_format_settings);

            /// Save previous progress callback if any. TODO Do it more conveniently.
            auto previous_progress_callback = context->getProgressCallback();

            /// NOTE Progress callback takes shared ownership of 'out'.
            streams.in->setProgressCallback([out, previous_progress_callback](const Progress & progress) {
                if (previous_progress_callback)
                    previous_progress_callback(progress);
                out->onProgress(progress);
            });

            if (set_result_details)
                set_result_details(
                    context->getClientInfo().current_query_id, out->getContentType(), format_name, DateLUT::instance().getTimeZone());

            copyData(
                *streams.in, *out, []() { return false; }, [&out](const Block &) { out->flush(); });
        }
        else if (pipeline.initialized())
        {
            const ASTQueryWithOutput * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get());

            WriteBuffer * out_buf = &ostr;
            std::optional<WriteBufferFromFile> out_file_buf;
            if (ast_query_with_output && ast_query_with_output->out_file)
            {
                if (!allow_into_outfile)
                    throw Exception("INTO OUTFILE is not allowed", ErrorCodes::INTO_OUTFILE_NOT_ALLOWED);

                const auto & out_file = typeid_cast<const ASTLiteral &>(*ast_query_with_output->out_file).value.safeGet<std::string>();
                out_file_buf.emplace(out_file, DBMS_DEFAULT_BUFFER_SIZE, O_WRONLY | O_EXCL | O_CREAT);
                out_buf = &*out_file_buf;
            }

            String format_name = ast_query_with_output && (ast_query_with_output->format != nullptr)
                ? getIdentifierName(ast_query_with_output->format)
                : context->getDefaultFormat();

            if (!pipeline.isCompleted())
            {
                pipeline.addSimpleTransform([](const Block & header) { return std::make_shared<MaterializingTransform>(header); });

                auto out = FormatFactory::instance().getOutputFormatParallelIfPossible(
                    format_name, *out_buf, pipeline.getHeader(), context, {}, output_format_settings);
                out->setAutoFlush();

                /// Save previous progress callback if any. TODO Do it more conveniently.
                auto previous_progress_callback = context->getProgressCallback();

                /// NOTE Progress callback takes shared ownership of 'out'.
                pipeline.setProgressCallback([out, previous_progress_callback](const Progress & progress) {
                    if (previous_progress_callback)
                        previous_progress_callback(progress);
                    out->onProgress(progress);
                });

                if (set_result_details)
                    set_result_details(
                        context->getClientInfo().current_query_id, out->getContentType(), format_name, DateLUT::instance().getTimeZone());

                pipeline.setOutputFormat(std::move(out));
            }
            else
            {
                pipeline.setProgressCallback(context->getProgressCallback());
            }

            {
                auto executor = pipeline.execute();
                executor->execute(pipeline.getNumThreads());
            }
        }
    }
    catch (...)
    {
        try
        {
            streams.onException();
            exception = std::current_exception();
        }
        catch (...)
        {
            exception = std::current_exception();
        }
    }

    if (exception)
    {
        std::rethrow_exception(exception);
    }

    streams.onFinish();
}

bool isQueryInInteractiveSession(const ContextPtr & context, [[maybe_unused]] const ASTPtr & query)
{
    return context->hasSessionContext() && (context->getSessionContext().get() != context.get())
        && context->getSessionContext()->getCurrentTransaction() != nullptr;
}

bool isDDLQuery([[maybe_unused]] const ContextPtr & context, const ASTPtr & query)
{
    auto * alter = query->as<ASTAlterQuery>();
    if (alter)
    {
        auto * command_list = alter->command_list;
        /// ATTACH PARTS FROM `dir` and ATTACH DETACHED PARTITION can be considered as DML
        if (command_list && command_list->children.size() == 1
            && (command_list->children[0]->as<ASTAlterCommand>()->attach_from_detached
                || command_list->children[0]->as<ASTAlterCommand>()->parts))
            return false;

        /// DROP PARTITION and DROP PARTITION WHERE without DETACH can be considered as DML
        if (command_list && command_list->children.size() == 1
            && ((command_list->children[0]->as<ASTAlterCommand>()->type == ASTAlterCommand::Type::DROP_PARTITION
                 || command_list->children[0]->as<ASTAlterCommand>()->type == ASTAlterCommand::Type::DROP_PARTITION_WHERE)
                && !command_list->children[0]->as<ASTAlterCommand>()->detach))
            return false;

        /// All other ATTACH considered DDL
        return true;
    }

    auto * create = query->as<ASTCreateQuery>();
    auto * drop = query->as<ASTDropQuery>();
    auto * rename = query->as<ASTRenameQuery>();

    return create || (drop && drop->kind != ASTDropQuery::Kind::Truncate) || rename;
}

bool isAsyncMode(ContextMutablePtr context)
{
    return context->getClientInfo().query_kind == ClientInfo::QueryKind::INITIAL_QUERY
        && context->getServerType() == ServerType::cnch_server && context->getSettings().enable_async_execution;
}

void updateAsyncQueryStatus(
    ContextMutablePtr context,
    const String & async_query_id,
    const String & query_id,
    const AsyncQueryStatus::Status & status,
    const String & error_msg)
{
    AsyncQueryStatus async_query_status;
    if (!context->getCnchCatalog()->tryGetAsyncQueryStatus(async_query_id, async_query_status))
    {
        LOG_WARNING(
            &Poco::Logger::get("executeQuery"), "async query status not found, insert new one with async_query_id: {}", async_query_id);
        async_query_status.set_id(async_query_id);
        async_query_status.set_query_id(query_id);
    }
    async_query_status.set_status(status);
    async_query_status.set_update_time(time(nullptr));

    if (!error_msg.empty() && status == AsyncQueryStatus::Failed)
    {
        async_query_status.set_error_msg(error_msg);
    }

    context->getCnchCatalog()->setAsyncQueryStatus(async_query_id, async_query_status);
}

void executeHttpQueryInAsyncMode(
    String & query1,
    ASTPtr ast1,
    ContextMutablePtr c,
    WriteBuffer & ostr1,
    ReadBuffer * istr1,
    bool has_query_tail,
    const std::optional<FormatSettings> & f,
    std::function<void(const String &, const String &, const String &, const String &)> set_result_details)
{
    const auto * ast_query_with_output1 = dynamic_cast<const ASTQueryWithOutput *>(ast1.get());
    String format_name1 = ast_query_with_output1 && (ast_query_with_output1->format != nullptr)
        ? getIdentifierName(ast_query_with_output1->format)
        : c->getDefaultFormat();
    if (set_result_details)
        set_result_details(
            c->getClientInfo().current_query_id, "text/plain; charset=UTF-8", format_name1, DateLUT::instance().getTimeZone());

    c->getAsyncQueryManager()->insertAndRun(
        query1,
        ast1,
        c,
        istr1,
        [c, &ostr1, &f](const String & id) {
            MutableColumnPtr table_column_mut = ColumnString::create();
            table_column_mut->insert(id);
            Block res;
            res.insert(ColumnWithTypeAndName(std::move(table_column_mut), std::make_shared<DataTypeString>(), "async_query_id"));

            auto out = FormatFactory::instance().getOutputFormatParallelIfPossible(c->getDefaultFormat(), ostr1, res, c, {}, f);

            out->write(res);
            out->flush();
        },
        [f, has_query_tail](String & query, ASTPtr ast, ContextMutablePtr context, ReadBuffer * istr) {
            ASTPtr ast_output;
            BlockIO streams;
            try
            {
                std::tie(ast_output, streams) = executeQueryImpl(
                    query.data(), query.data() + query.size(), ast, context, false, QueryProcessingStage::Complete, has_query_tail, istr);
                auto & pipeline = streams.pipeline;
                if (streams.in)
                {
                    const auto * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get());

                    std::shared_ptr<WriteBuffer> out_buf;
                    std::optional<String> out_path;
                    bool write_to_hdfs = false;
#if USE_HDFS
                    std::unique_ptr<WriteBufferFromHDFS> out_hdfs_raw;
                    std::optional<ZlibDeflatingWriteBuffer> out_hdfs_buf;
#endif
                    if (ast_query_with_output && ast_query_with_output->out_file)
                    {
                        out_path.emplace(typeid_cast<const ASTLiteral &>(*ast_query_with_output->out_file).value.safeGet<std::string>());
                        const Poco::URI out_uri(*out_path);
                        const String & scheme = out_uri.getScheme();

                        if (scheme.empty())
                        {
                            throw Exception("INTO OUTFILE is not allowed", ErrorCodes::INTO_OUTFILE_NOT_ALLOWED);
                        }
#if USE_HDFS
                        else if (DB::isHdfsOrCfsScheme(scheme))
                        {
                            out_hdfs_raw = std::make_unique<WriteBufferFromHDFS>(
                                *out_path, context->getHdfsConnectionParams(), context->getSettingsRef().max_hdfs_write_buffer_size);
                            out_buf = std::make_shared<ZlibDeflatingWriteBuffer>(
                                std::move(out_hdfs_raw), CompressionMethod::Gzip, Z_DEFAULT_COMPRESSION);
                            write_to_hdfs = true;
                        }
#endif
                        else
                        {
                            throw Exception(
                                "Path: " + *out_path + " is illegal, only support write query result to local file or tos",
                                ErrorCodes::CANNOT_PARSE_DOMAIN_VALUE_FROM_STRING);
                        }
                    }

                    String format_name = ast_query_with_output && (ast_query_with_output->format != nullptr)
                        ? getIdentifierName(ast_query_with_output->format)
                        : context->getDefaultFormat();

                    BlockOutputStreamPtr out = write_to_hdfs ? FormatFactory::instance().getOutputStreamParallelIfPossible(
                                                   format_name, *out_buf, streams.in->getHeader(), context, {}, f)
                                                             : std::make_shared<NullBlockOutputStream>(Block{});

                    copyData(
                        *streams.in, *out, []() { return false; }, [&out](const Block &) { out->flush(); });
                }
                else if (pipeline.initialized())
                {
                    const ASTQueryWithOutput * ast_query_with_output = dynamic_cast<const ASTQueryWithOutput *>(ast.get());

                    if (ast_query_with_output && ast_query_with_output->out_file)
                    {
                        throw Exception("INTO OUTFILE is not allowed in http async mode", ErrorCodes::INTO_OUTFILE_NOT_ALLOWED);
                    }

                    String format_name = ast_query_with_output && (ast_query_with_output->format != nullptr)
                        ? getIdentifierName(ast_query_with_output->format)
                        : context->getDefaultFormat();

                    if (!pipeline.isCompleted())
                    {
                        pipeline.addSimpleTransform([](const Block & header) { return std::make_shared<MaterializingTransform>(header); });

                        auto out = FormatFactory::instance().getOutputFormatParallelIfPossible(
                            "Null", *new WriteBuffer(nullptr, 0), pipeline.getHeader(), context, {}, f);

                        pipeline.setOutputFormat(std::move(out));
                    }
                    else
                    {
                        pipeline.setProgressCallback(context->getProgressCallback());
                    }

                    {
                        auto executor = pipeline.execute();
                        executor->execute(pipeline.getNumThreads());
                    }
                }
            }
            catch (...)
            {
                streams.onException();
                if (!streams.exception_callback)
                    updateAsyncQueryStatus(
                        context,
                        context->getAsyncQueryId(),
                        context->getCurrentQueryId(),
                        AsyncQueryStatus::Failed,
                        getCurrentExceptionMessage(false));
                throw;
            }

            streams.onFinish();
        });
}

}
