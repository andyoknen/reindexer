#include <unordered_map>
#include <unordered_set>
#include "replication_load_api.h"
#include "replicator/walrecord.h"

TEST_F(ReplicationLoadApi, UpdatesFilteringByNs) {
	InitNs();

	auto master = GetSrv(masterId_);
	auto& api = master->api;

	const auto kNs1Name = "ns1"_sv;
	const auto kNs2Name = "ns2"_sv;
	const auto kNs3Name = "ns3"_sv;
	const auto kSomeName = "some"_sv;
	const auto kSome1Name = "some1"_sv;

	auto awaitNamespaces = [](const UpdatesReciever& reciever, size_t count) {
		std::chrono::milliseconds time{0};
		auto cycleTime = std::chrono::milliseconds(50);
		while (reciever.Counters().size() != count) {
			time += cycleTime;
			if (time >= std::chrono::seconds(5)) {
				reciever.Dump();
				return false;
			}
			std::this_thread::sleep_for(cycleTime);
		}
		return true;
	};

	auto awaitItems = [](const UpdatesReciever& reciever, string_view ns, size_t count) {
		std::chrono::milliseconds time{0};
		auto cycleTime = std::chrono::milliseconds(50);
		do {
			auto counters = reciever.Counters();
			time += cycleTime;
			if (time >= std::chrono::seconds(5)) {
				reciever.Dump();
				return false;
			}

			auto found = counters.find(ns);
			if (found != counters.end() && found.value() == count) {
				return true;
			}
			std::this_thread::sleep_for(cycleTime);
		} while (true);
	};

	UpdatesReciever reciever1;	// Recieves updates for ns 'some1' and 'ns1'
	{
		UpdatesFilters filters;
		{
			UpdatesFilters::Filter filter;
			filters.AddFilter(kSome1Name, std::move(filter));
		}
		auto err = api.reindexer->SubscribeUpdates(&reciever1, filters);
		ASSERT_TRUE(err.ok()) << err.what();
		UpdatesFilters filters1;
		{
			UpdatesFilters::Filter filter;
			filters1.AddFilter(kNs1Name, std::move(filter));
		}
		err = api.reindexer->SubscribeUpdates(&reciever1, filters1, SubscriptionOpts().IncrementSubscription());
		ASSERT_TRUE(err.ok()) << err.what();
	}

	{
		const size_t count = 50;
		FillData(count);
		ASSERT_TRUE(awaitNamespaces(reciever1, 1));
		ASSERT_TRUE(awaitItems(reciever1, kSome1Name, count));
		reciever1.Reset();
	}

	UpdatesReciever reciever2;	// Recieves all the updates
	{
		UpdatesFilters filters;
		auto err = api.reindexer->SubscribeUpdates(&reciever2, filters);
		ASSERT_TRUE(err.ok()) << err.what();
		UpdatesFilters filters1;
		err = api.reindexer->SubscribeUpdates(&reciever2, filters1, SubscriptionOpts().IncrementSubscription());
		ASSERT_TRUE(err.ok()) << err.what();
	}

	UpdatesReciever reciever3;	// Recieves updates for ns 'some'
	{
		UpdatesFilters filters;
		{
			UpdatesFilters::Filter filter;
			filters.AddFilter(kSomeName, std::move(filter));
		}
		auto err = api.reindexer->SubscribeUpdates(&reciever3, filters);
		ASSERT_TRUE(err.ok()) << err.what();
	}

	{
		const size_t count = 100;
		FillData(count);
		ASSERT_TRUE(awaitNamespaces(reciever1, 1));
		ASSERT_TRUE(awaitItems(reciever1, kSome1Name, count));
		reciever1.Reset();

		ASSERT_TRUE(awaitNamespaces(reciever2, 2));
		ASSERT_TRUE(awaitItems(reciever2, kSome1Name, count));
		ASSERT_TRUE(awaitItems(reciever2, kSomeName, count));
		reciever2.Reset();

		ASSERT_TRUE(awaitNamespaces(reciever3, 1));
		ASSERT_TRUE(awaitItems(reciever3, kSomeName, count));
		reciever3.Reset();
	}

	auto err = api.reindexer->OpenNamespace(kNs1Name);
	ASSERT_TRUE(err.ok()) << err.what();
	err = api.reindexer->OpenNamespace(kNs2Name);
	ASSERT_TRUE(err.ok()) << err.what();
	err = api.reindexer->RenameNamespace(kNs1Name, string(kNs3Name));
	ASSERT_TRUE(err.ok()) << err.what();
	err = api.reindexer->DropNamespace(kNs2Name);
	ASSERT_TRUE(err.ok()) << err.what();

	{
		ASSERT_TRUE(awaitNamespaces(reciever1, 2));
		ASSERT_TRUE(awaitItems(reciever1, kNs1Name, 2));
		ASSERT_TRUE(awaitItems(reciever1, kNs2Name, 2));
		reciever1.Reset();

		ASSERT_TRUE(awaitNamespaces(reciever2, 2));
		ASSERT_TRUE(awaitItems(reciever2, kNs1Name, 2));
		ASSERT_TRUE(awaitItems(reciever2, kNs2Name, 2));
		reciever2.Reset();

		ASSERT_TRUE(awaitNamespaces(reciever3, 2));
		ASSERT_TRUE(awaitItems(reciever3, kNs1Name, 2));
		ASSERT_TRUE(awaitItems(reciever3, kNs2Name, 2));
		reciever3.Reset();
	}

	err = api.reindexer->UnsubscribeUpdates(&reciever1);
	ASSERT_TRUE(err.ok()) << err.what();
	err = api.reindexer->UnsubscribeUpdates(&reciever2);
	ASSERT_TRUE(err.ok()) << err.what();
	err = api.reindexer->UnsubscribeUpdates(&reciever3);
	ASSERT_TRUE(err.ok()) << err.what();

	{
		const size_t count = 50;
		FillData(count);
		sleep(2);
		ASSERT_TRUE(awaitNamespaces(reciever1, 0));
		ASSERT_TRUE(awaitNamespaces(reciever2, 0));
		ASSERT_TRUE(awaitNamespaces(reciever3, 0));
	}
}

TEST_F(ReplicationLoadApi, Base) {
	InitNs();
	stop = false;
	SetWALSize(masterId_, 100000, "some");
	FillData(1000);

	std::thread destroyer([this]() {
		int count = 0;
		while (!stop) {
			if (!(count % 30)) {
				int i = rand() % kDefaultServerCount;
				RestartServer(i);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	});

	SetWALSize(masterId_, 50000, "some");
	for (size_t i = 0; i < 2; ++i) {
		if (i % 3 == 0) DeleteFromMaster();
		SetWALSize(masterId_, (int64_t(i) + 1) * 25000, "some1");
		FillData(1000);
		SetWALSize(masterId_, (int64_t(i) + 1) * 50000, "some");
		SimpleSelect(0);
	}

	SetWALSize(masterId_, 50000, "some1");

	stop = true;
	destroyer.join();

	ForceSync();
	WaitSync("some");
	WaitSync("some1");
}

TEST_F(ReplicationLoadApi, WALResizeStaticData) {
	InitNs();

	const std::string nsName("some");
	auto master = GetSrv(masterId_)->api.reindexer;
	ASSERT_NO_FATAL_FAILURE(SetWALSize(masterId_, 1000, nsName));
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(0)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 3);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(2)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 1);
	}

	FillData(500);

	client::QueryResults qrLast100_1(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
	client::QueryResults qrLast100_2(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
	client::QueryResults qrLast100_3(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);

	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(0)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 503);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(502)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 1);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(503)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
		EXPECT_EQ(qr.Count(), 0);
	}
	{
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(402)), qrLast100_1);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qrLast100_1.Count(), 101);
	}
	ASSERT_NO_FATAL_FAILURE(SetWALSize(masterId_, 100, nsName));
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(401)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
	}
	{
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(402)), qrLast100_2);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qrLast100_2.Count(), 101);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(502)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 1);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(503)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
		EXPECT_EQ(qr.Count(), 0);
	}
	ASSERT_NO_FATAL_FAILURE(SetWALSize(masterId_, 2000, nsName));
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(401)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
		EXPECT_EQ(qr.Count(), 0);
	}
	{
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(402)), qrLast100_3);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qrLast100_3.Count(), 101);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(502)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 1);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(503)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
		EXPECT_EQ(qr.Count(), 0);
	}

	auto qrToSet = [](const client::QueryResults& qr) {
		std::unordered_set<string> items;
		WrSerializer ser;
		for (auto& item : qr) {
			if (item.IsRaw()) {
				reindexer::WALRecord rec(item.GetRaw());
				EXPECT_EQ(rec.type, WalReplState);
			} else {
				ser.Reset();
				auto err = item.GetCJSON(ser, false);
				EXPECT_TRUE(err.ok());
				items.emplace(ser.Slice());
			}
		}
		return items;
	};
	auto items_1 = qrToSet(qrLast100_1);
	auto items_2 = qrToSet(qrLast100_2);
	auto items_3 = qrToSet(qrLast100_3);
	ASSERT_EQ(items_1.size(), 100);
	ASSERT_TRUE(items_1 == items_2);
	ASSERT_TRUE(items_1 == items_3);
}

TEST_F(ReplicationLoadApi, WALResizeDynamicData) {
	InitNs();

	const std::string nsName("some");
	ASSERT_NO_FATAL_FAILURE(SetWALSize(masterId_, 1000, nsName));
	FillData(500);

	client::QueryResults qrLast100_1(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
	client::QueryResults qrLast100_2(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
	client::QueryResults qrLast100_3(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);

	auto master = GetSrv(masterId_)->api.reindexer;
	ASSERT_NO_FATAL_FAILURE(SetWALSize(masterId_, 100, nsName));
	FillData(50);
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(451)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(452)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 101);
	}
	ASSERT_NO_FATAL_FAILURE(SetWALSize(masterId_, 200, nsName));
	FillData(500);
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(851)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
		EXPECT_EQ(qr.Count(), 0);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(852)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 201);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(1052)), qr);
		EXPECT_TRUE(err.ok()) << err.what();
		EXPECT_EQ(qr.Count(), 1);
	}
	{
		client::QueryResults qr(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
		Error err = master->Select(Query(nsName).Where("#lsn", CondGt, int64_t(1053)), qr);
		EXPECT_EQ(err.code(), errOutdatedWAL) << err.what();
		EXPECT_EQ(qr.Count(), 0);
	}
}

TEST_F(ReplicationLoadApi, DISABLED_BasicTestNoMasterRestart) {
	InitNs();
	stop = false;
	FillData(1000);

	std::thread destroyer([this]() {
		while (!stop) {
			RestartServer(rand() % 3 + 1);
			std::this_thread::sleep_for(std::chrono::seconds(3));
		}
	});

	for (size_t i = 0; i < 10; ++i) {
		if (i % 3 == 0) DeleteFromMaster();
		FillData(1000);
		SimpleSelect(0);
		SimpleSelect(rand() % 3 + 1);
	}

	stop = true;
	destroyer.join();
}

TEST_F(ReplicationLoadApi, SingleSlaveTest) {
	InitNs();
	stop = false;
	FillData(1000);

	std::thread writingThread([this]() {
		while (!stop) {
			FillData(1000);
		}
	});

	std::thread removingThread([this]() {
		size_t counter = 0;
		while (!stop) {
			std::this_thread::sleep_for(std::chrono::seconds(3));
			int i = rand() % 2;
			counter++;

			RestartServer(i);
			if (counter % 3 == 0) DeleteFromMaster();
		}
	});

	for (size_t i = 0; i < 2; ++i) {
		SimpleSelect(0);
		SetWALSize(masterId_, (int64_t(i) + 1) * 1000, "some1");
		SetWALSize(masterId_, (int64_t(i) + 1) * 1000, "some");
		std::this_thread::sleep_for(std::chrono::seconds(3));
	}

	stop = true;
	writingThread.join();
	removingThread.join();
	ForceSync();
	WaitSync("some");
	WaitSync("some1");
}

TEST_F(ReplicationLoadApi, ConfigSync) {
	ReplicationConfigTest config("slave", true, false, 0, "cproto://127.0.0.1:6534/0", "slave_1");
	const size_t kTestSlaveID = 2;
	RestartWithConfigFile(kTestSlaveID,
						  "role: slave\n"
						  "master_dsn: cproto://127.0.0.1:6534/0\n"
						  "app_name: slave_1\n"
						  "cluster_id: 2\n"
						  "force_sync_on_logic_error: true\n"
						  "force_sync_on_wrong_data_hash: false\n"
						  "namespaces: []");
	CheckSlaveConfigFile(kTestSlaveID, config);
	config = ReplicationConfigTest("slave", false, true, 0, "cproto://127.0.0.1:6534/12345", "slave_1", {"ns1", "ns2"});
	SetServerConfig(kTestSlaveID, config);
	CheckSlaveConfigFile(kTestSlaveID, config);
	config = ReplicationConfigTest("slave", true, false, 0, "cproto://127.0.0.1:6534/999", "slave_1");
	SetServerConfig(kTestSlaveID, config);
	CheckSlaveConfigFile(kTestSlaveID, config);
	std::this_thread::sleep_for(std::chrono::seconds(2));  // In case if OS doesn't have nanosecods in stat result

	GetSrv(kTestSlaveID)
		->WriteServerConfig(
			"role: slave\n"
			"master_dsn: cproto://127.0.0.1:6534/somensname\n"
			"app_name: slave_1\n"
			"cluster_id: 2\n"
			"force_sync_on_logic_error: false\n"
			"force_sync_on_wrong_data_hash: true\n"
			"namespaces:\n"
			"  - ns1\n"
			"  - ns3\n");
	config = ReplicationConfigTest("slave", false, true, 0, "cproto://127.0.0.1:6534/somensname", "slave_1", {"ns1", "ns3"});
	CheckSlaveConfigNamespace(kTestSlaveID, config, std::chrono::seconds(3));
}

TEST_F(ReplicationLoadApi, DynamicRoleSwitch) {
	InitNs();
	for (size_t i = 1; i < 8; i++) {
		FillData(2000);
		WaitSync("some");
		WaitSync("some1");
		SwitchMaster(i % kDefaultServerCount);
	}
}
