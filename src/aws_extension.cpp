#define DUCKDB_EXTENSION_MAIN

#include "aws_secret.hpp"
#include "aws_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/extension_util.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>

namespace duckdb {

//! Set the DuckDB AWS Credentials using the DefaultAWSCredentialsProviderChain
static AwsSetCredentialsResult TrySetAwsCredentials(DBConfig &config, const string &profile, bool set_region) {
	Aws::SDKOptions options;
	Aws::InitAPI(options);
	Aws::Auth::AWSCredentials credentials;

	if (!profile.empty()) {
		// The user has specified a specific profile they want to use instead of the current profile specified by the
		// system
		Aws::Auth::ProfileConfigFileAWSCredentialsProvider provider(profile.c_str());
		credentials = provider.GetAWSCredentials();
	} else {
		Aws::Auth::DefaultAWSCredentialsProviderChain provider;
		credentials = provider.GetAWSCredentials();
	}

	auto s3_config = Aws::Client::ClientConfiguration(profile.c_str());
	auto region = s3_config.region;

	// TODO: We would also like to get the endpoint here, but it's currently not supported by the AWS SDK:
	// 		 https://github.com/aws/aws-sdk-cpp/issues/2587

	AwsSetCredentialsResult ret;
	if (!credentials.IsExpiredOrEmpty()) {
		config.SetOption("s3_access_key_id", Value(credentials.GetAWSAccessKeyId()));
		config.SetOption("s3_secret_access_key", Value(credentials.GetAWSSecretKey()));
		config.SetOption("s3_session_token", Value(credentials.GetSessionToken()));
		ret.set_access_key_id = credentials.GetAWSAccessKeyId();
		ret.set_secret_access_key = credentials.GetAWSSecretKey();
		ret.set_session_token = credentials.GetSessionToken();
	}

	if (!region.empty() && set_region) {
		config.SetOption("s3_region", Value(region));
		ret.set_region = region;
	}

	Aws::ShutdownAPI(options);
	return ret;
}

struct SetAWSCredentialsFunctionData : public TableFunctionData {
	string profile_name;
	bool finished = false;
	bool set_region = true;
	bool redact_secret = true;
};

static unique_ptr<FunctionData> LoadAWSCredentialsBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<SetAWSCredentialsFunctionData>();

	for (const auto &option : input.named_parameters) {
		if (option.first == "set_region") {
			result->set_region = BooleanValue::Get(option.second);
		} else if (option.first == "redact_secret") {
			result->redact_secret = BooleanValue::Get(option.second);
		}
	}

	if (input.inputs.size() >= 1) {
		result->profile_name = input.inputs[0].ToString();
	}

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("loaded_access_key_id");

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("loaded_secret_access_key");

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("loaded_session_token");

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("loaded_region");

	return std::move(result);
}

static void LoadAWSCredentialsFun(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (SetAWSCredentialsFunctionData &)*data_p.bind_data;
	if (data.finished) {
		return;
	}

	if (!Catalog::TryAutoLoad(context, "httpfs")) {
		throw MissingExtensionException("httpfs extension is required for load_aws_credentials");
	}

	auto load_result = TrySetAwsCredentials(DBConfig::GetConfig(context), data.profile_name, data.set_region);

	// Set return values for all modified params
	output.SetValue(0, 0, load_result.set_access_key_id.empty() ? Value(nullptr) : load_result.set_access_key_id);
	if (data.redact_secret && !load_result.set_secret_access_key.empty()) {
		output.SetValue(1, 0, "<redacted>");
	} else {
		output.SetValue(1, 0,
		                load_result.set_secret_access_key.empty() ? Value(nullptr) : load_result.set_secret_access_key);
	}
	output.SetValue(2, 0, load_result.set_session_token.empty() ? Value(nullptr) : load_result.set_session_token);
	output.SetValue(3, 0, load_result.set_region.empty() ? Value(nullptr) : load_result.set_region);

	output.SetCardinality(1);

	data.finished = true;
}

static void LoadInternal(DuckDB &db) {
	TableFunctionSet function_set("load_aws_credentials");
	auto base_fun = TableFunction("load_aws_credentials", {}, LoadAWSCredentialsFun, LoadAWSCredentialsBind);
	auto profile_fun =
	    TableFunction("load_aws_credentials", {LogicalTypeId::VARCHAR}, LoadAWSCredentialsFun, LoadAWSCredentialsBind);

	base_fun.named_parameters["set_region"] = LogicalTypeId::BOOLEAN;
	base_fun.named_parameters["redact_secret"] = LogicalTypeId::BOOLEAN;
	profile_fun.named_parameters["set_region"] = LogicalTypeId::BOOLEAN;
	profile_fun.named_parameters["redact_secret"] = LogicalTypeId::BOOLEAN;

	function_set.AddFunction(base_fun);
	function_set.AddFunction(profile_fun);

	ExtensionUtil::RegisterFunction(*db.instance, function_set);

	CreateAwsSecretFunctions::Register(*db.instance);
}

void AwsExtension::Load(DuckDB &db) {
	LoadInternal(db);
}
std::string AwsExtension::Name() {
	return "aws";
}

std::string AwsExtension::Version() const {
#ifdef EXT_VERSION_AWS
	return EXT_VERSION_AWS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void aws_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::AwsExtension>();
}

DUCKDB_EXTENSION_API const char *aws_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
