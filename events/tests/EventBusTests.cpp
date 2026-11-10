// Copyright GALAXY_PROJECT. All Rights Reserved.
// ============================================================================
// Unit tests for the EventBus deterministic dispatch system.
// ============================================================================

#include "EventBusTests.h"

#include "Infrastructure/EventBus.h"
#include "TestUtilities.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// ============================================================================
// Test Event Types
// ============================================================================

namespace
{
	struct FTestEventA : public TEvent<FTestEventA>
	{
		int32 Value = 0;
		explicit FTestEventA(int32 InValue = 0) : Value(InValue) {}
		FString GetEventName() const override { return TEXT("TestEventA"); }
	};

	struct FTestEventB : public TEvent<FTestEventB>
	{
		FString Message;
		explicit FTestEventB(const FString& InMessage = TEXT("")) : Message(InMessage) {}
		FString GetEventName() const override { return TEXT("TestEventB"); }
	};

	struct FTestEntityEvent : public TEntityEvent<FTestEntityEvent>
	{
		float Damage = 0.0f;
		explicit FTestEntityEvent(uint64 InEntityId = 0, float InDamage = 0.0f)
			: Damage(InDamage)
		{
			EntityId = InEntityId;
		}
		FString GetEventName() const override { return TEXT("TestEntityEvent"); }
	};
}

// ============================================================================
// Basic Subscribe/Emit/Flush Cycle
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusBasicCycleTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.BasicCycle",
	GALAXY_TEST_FLAGS
)

bool FEventBusBasicCycleTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 Received = 0;
	auto Handle = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E)
	{
		Received = E.Value;
	});

	// Emit queued -- should not dispatch yet
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 42);
	TestEqual(TEXT("Should not dispatch before Flush"), Received, 0);
	TestEqual(TEXT("Queue should have 1 event"), Bus.GetQueuedCount(), 1);

	// Flush dispatches
	int32 Dispatched = Bus.Flush();
	TestEqual(TEXT("Should dispatch 1 event"), Dispatched, 1);
	TestEqual(TEXT("Handler should receive value 42"), Received, 42);
	TestTrue(TEXT("Queue should be empty after Flush"), Bus.IsQueueEmpty());

	Bus.Unsubscribe(Handle);
	return true;
}

// ============================================================================
// Immediate Dispatch Mode
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusImmediateTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.ImmediateDispatch",
	GALAXY_TEST_FLAGS
)

bool FEventBusImmediateTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 Received = 0;
	auto Handle = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E)
	{
		Received = E.Value;
	});

	// Immediate -- dispatches without Flush
	Bus.EmitImmediate(MakeUnique<FTestEventA>(99));
	TestEqual(TEXT("Immediate should dispatch instantly"), Received, 99);
	TestTrue(TEXT("Queue should remain empty"), Bus.IsQueueEmpty());

	Bus.Unsubscribe(Handle);
	return true;
}

// ============================================================================
// Priority Ordering
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusPriorityTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.PriorityOrdering",
	GALAXY_TEST_FLAGS
)

bool FEventBusPriorityTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	TArray<FString> Order;

	auto H1 = Bus.Subscribe<FTestEventA>([&Order](const FTestEventA&)
	{
		Order.Add(TEXT("Normal"));
	}, EEventPriority::Normal);

	auto H2 = Bus.Subscribe<FTestEventA>([&Order](const FTestEventA&)
	{
		Order.Add(TEXT("System"));
	}, EEventPriority::System);

	auto H3 = Bus.Subscribe<FTestEventA>([&Order](const FTestEventA&)
	{
		Order.Add(TEXT("Low"));
	}, EEventPriority::Low);

	auto H4 = Bus.Subscribe<FTestEventA>([&Order](const FTestEventA&)
	{
		Order.Add(TEXT("High"));
	}, EEventPriority::High);

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued);
	Bus.Flush();

	TestEqual(TEXT("Should have 4 invocations"), Order.Num(), 4);
	if (Order.Num() == 4)
	{
		TestEqual(TEXT("System first"), Order[0], TEXT("System"));
		TestEqual(TEXT("High second"), Order[1], TEXT("High"));
		TestEqual(TEXT("Normal third"), Order[2], TEXT("Normal"));
		TestEqual(TEXT("Low fourth"), Order[3], TEXT("Low"));
	}

	Bus.Unsubscribe(H1);
	Bus.Unsubscribe(H2);
	Bus.Unsubscribe(H3);
	Bus.Unsubscribe(H4);
	return true;
}

// ============================================================================
// Unsubscribe Stops Dispatch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusUnsubscribeTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.Unsubscribe",
	GALAXY_TEST_FLAGS
)

bool FEventBusUnsubscribeTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 Count = 0;
	auto Handle = Bus.Subscribe<FTestEventA>([&Count](const FTestEventA&)
	{
		Count++;
	});

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued);
	Bus.Flush();
	TestEqual(TEXT("Should receive first event"), Count, 1);

	bool bRemoved = Bus.Unsubscribe(Handle);
	TestTrue(TEXT("Unsubscribe should succeed"), bRemoved);

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued);
	Bus.Flush();
	TestEqual(TEXT("Should not receive after unsubscribe"), Count, 1);

	return true;
}

// ============================================================================
// Multiple Event Types Are Independent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusMultiTypeTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.MultipleEventTypes",
	GALAXY_TEST_FLAGS
)

bool FEventBusMultiTypeTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 ACount = 0;
	int32 BCount = 0;

	auto HA = Bus.Subscribe<FTestEventA>([&ACount](const FTestEventA&) { ACount++; });
	auto HB = Bus.Subscribe<FTestEventB>([&BCount](const FTestEventB&) { BCount++; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("hello"));
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.Flush();

	TestEqual(TEXT("A handler should fire twice"), ACount, 2);
	TestEqual(TEXT("B handler should fire once"), BCount, 1);

	Bus.Unsubscribe(HA);
	Bus.Unsubscribe(HB);
	return true;
}

// ============================================================================
// FIFO Ordering Within Same Priority
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusFIFOTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.FIFOOrdering",
	GALAXY_TEST_FLAGS
)

bool FEventBusFIFOTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	TArray<int32> Received;
	auto Handle = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E)
	{
		Received.Add(E.Value);
	});

	for (int32 i = 0; i < 10; ++i)
	{
		Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, i);
	}
	Bus.Flush();

	TestEqual(TEXT("Should receive 10 events"), Received.Num(), 10);
	for (int32 i = 0; i < Received.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("Event %d should be in FIFO order"), i), Received[i], i);
	}

	Bus.Unsubscribe(Handle);
	return true;
}

// ============================================================================
// ClearQueue Drops Without Dispatch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusClearQueueTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.ClearQueue",
	GALAXY_TEST_FLAGS
)

bool FEventBusClearQueueTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 Count = 0;
	auto Handle = Bus.Subscribe<FTestEventA>([&Count](const FTestEventA&) { Count++; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	TestEqual(TEXT("Queue should have 2 events"), Bus.GetQueuedCount(), 2);

	int32 Cleared = Bus.ClearQueue();
	TestEqual(TEXT("Should clear 2 events"), Cleared, 2);
	TestTrue(TEXT("Queue should be empty"), Bus.IsQueueEmpty());

	Bus.Flush();
	TestEqual(TEXT("Handler should not have fired"), Count, 0);

	Bus.Unsubscribe(Handle);
	return true;
}

// ============================================================================
// Handler Count Tracking
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusHandlerCountTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.HandlerCount",
	GALAXY_TEST_FLAGS
)

bool FEventBusHandlerCountTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	TestEqual(TEXT("No handlers initially"), Bus.GetHandlerCount<FTestEventA>(), 0);

	auto H1 = Bus.Subscribe<FTestEventA>([](const FTestEventA&) {});
	TestEqual(TEXT("1 handler after subscribe"), Bus.GetHandlerCount<FTestEventA>(), 1);

	auto H2 = Bus.Subscribe<FTestEventA>([](const FTestEventA&) {});
	TestEqual(TEXT("2 handlers"), Bus.GetHandlerCount<FTestEventA>(), 2);

	TestEqual(TEXT("B type still has 0"), Bus.GetHandlerCount<FTestEventB>(), 0);

	Bus.Unsubscribe(H1);
	TestEqual(TEXT("1 handler after unsubscribe"), Bus.GetHandlerCount<FTestEventA>(), 1);

	Bus.Unsubscribe(H2);
	TestEqual(TEXT("0 handlers"), Bus.GetHandlerCount<FTestEventA>(), 0);

	return true;
}

// ============================================================================
// Metadata Stamping (Tick + Sequence)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusMetadataTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.MetadataStamping",
	GALAXY_TEST_FLAGS
)

bool FEventBusMetadataTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(5);

	TArray<FEventMetadata> Metas;
	auto Handle = Bus.Subscribe<FTestEventA>([&Metas](const FTestEventA& E)
	{
		Metas.Add(E.GetMetadata());
	});

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, FName(TEXT("Combat")), 1);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, FName(TEXT("Combat")), 2);
	Bus.Flush();

	TestEqual(TEXT("Should have 2 metadata entries"), Metas.Num(), 2);
	if (Metas.Num() == 2)
	{
		TestEqual(TEXT("First event tick should be 5"), Metas[0].EmitTick, static_cast<uint64>(5));
		TestEqual(TEXT("Second event tick should be 5"), Metas[1].EmitTick, static_cast<uint64>(5));
		TestTrue(TEXT("Sequence should increment"), Metas[1].TickSequence > Metas[0].TickSequence);
		TestEqual(TEXT("Source domain should be Combat"), Metas[0].SourceDomain, FName(TEXT("Combat")));
	}

	Bus.Unsubscribe(Handle);
	return true;
}

// ============================================================================
// Entity Event Type
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusEntityEventTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.EntityEvent",
	GALAXY_TEST_FLAGS
)

bool FEventBusEntityEventTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	uint64 ReceivedEntityId = 0;
	float ReceivedDamage = 0.0f;
	auto Handle = Bus.Subscribe<FTestEntityEvent>([&ReceivedEntityId, &ReceivedDamage](const FTestEntityEvent& E)
	{
		ReceivedEntityId = E.EntityId;
		ReceivedDamage = E.Damage;
	});

	Bus.EmitNew<FTestEntityEvent>(EEventDispatchMode::Queued, NAME_None, static_cast<uint64>(42), 15.5f);
	Bus.Flush();

	TestEqual(TEXT("Entity ID should be 42"), ReceivedEntityId, static_cast<uint64>(42));
	TestUtilities::TestFloatNear(*this, TEXT("Damage should be ~15.5"), ReceivedDamage, 15.5f, 0.0001f);

	Bus.Unsubscribe(Handle);
	return true;
}

// ============================================================================
// Telemetry Tracking
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusTelemetryTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.Telemetry",
	GALAXY_TEST_FLAGS
)

bool FEventBusTelemetryTest::RunTest(const FString& Parameters)
{
	FEventBusConfig Config;
	Config.bEnableTelemetry = true;

	FEventBusManager Bus;
	Bus.Initialize(Config);

	auto Handle = Bus.Subscribe<FTestEventA>([](const FTestEventA&) {});

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 3);
	Bus.Flush();

	const FEventBusTelemetry& Telemetry = Bus.GetTelemetry();
	TestEqual(TEXT("TotalEmitted should be 3"), Telemetry.TotalEmitted, static_cast<uint64>(3));
	TestEqual(TEXT("TotalDispatched should be 3"), Telemetry.TotalDispatched, static_cast<uint64>(3));
	TestEqual(TEXT("TotalHandlerCalls should be 3"), Telemetry.TotalHandlerCalls, static_cast<uint64>(3));

	Bus.Unsubscribe(Handle);
	return true;
}

// ============================================================================
// Multiple Handlers Same Event Type
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusMultiHandlerTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.MultipleHandlers",
	GALAXY_TEST_FLAGS
)

bool FEventBusMultiHandlerTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 CountA = 0;
	int32 CountB = 0;
	int32 CountC = 0;

	auto H1 = Bus.Subscribe<FTestEventA>([&CountA](const FTestEventA&) { CountA++; });
	auto H2 = Bus.Subscribe<FTestEventA>([&CountB](const FTestEventA&) { CountB++; });
	auto H3 = Bus.Subscribe<FTestEventA>([&CountC](const FTestEventA&) { CountC++; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.Flush();

	TestEqual(TEXT("Handler A should fire once"), CountA, 1);
	TestEqual(TEXT("Handler B should fire once"), CountB, 1);
	TestEqual(TEXT("Handler C should fire once"), CountC, 1);

	Bus.Unsubscribe(H1);
	Bus.Unsubscribe(H2);
	Bus.Unsubscribe(H3);
	return true;
}

// ============================================================================
// Determinism: Same Input -> Same Output
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusDeterminismTest,
	"GALAXY_PROJECT.EventBus.Determinism.Unit.ReplayConsistency",
	GALAXY_TEST_FLAGS
)

bool FEventBusDeterminismTest::RunTest(const FString& Parameters)
{
	// Run the same sequence twice and verify identical handler invocation order
	auto RunSequence = []() -> TArray<int32>
	{
		TArray<int32> Results;

		FEventBusManager Bus;
		Bus.Initialize(FEventBusConfig{});
		Bus.SetCurrentTick(1);

		Bus.Subscribe<FTestEventA>([&Results](const FTestEventA& E)
		{
			Results.Add(E.Value * 10); // Multiply to distinguish handler
		}, EEventPriority::Normal);

		Bus.Subscribe<FTestEventA>([&Results](const FTestEventA& E)
		{
			Results.Add(E.Value * 100);
		}, EEventPriority::High);

		for (int32 i = 1; i <= 5; ++i)
		{
			Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, i);
		}
		const int32 Dispatched = Bus.Flush();
		if (Dispatched != 5)
		{
			Results.Reset();
		}

		return Results;
	};

	TArray<int32> Run1 = RunSequence();
	TArray<int32> Run2 = RunSequence();

	TestEqual(TEXT("Both runs should produce same count"), Run1.Num(), Run2.Num());
	for (int32 i = 0; i < FMath::Min(Run1.Num(), Run2.Num()); ++i)
	{
		TestEqual(FString::Printf(TEXT("Element %d should match"), i), Run1[i], Run2[i]);
	}

	const TArray<int32> Expected = { 100, 10, 200, 20, 300, 30, 400, 40, 500, 50 };
	TestEqual(TEXT("Run1 should produce expected priority/FIFO order"), Run1.Num(), Expected.Num());
	for (int32 i = 0; i < FMath::Min(Run1.Num(), Expected.Num()); ++i)
	{
		TestEqual(FString::Printf(TEXT("Expected element %d"), i), Run1[i], Expected[i]);
	}

	// Golden baseline -- detect dispatch ordering drift
	{
		uint64 BusChecksum = 0;
		for (int32 i = 0; i < Run1.Num(); ++i)
		{
			BusChecksum ^= GetTypeHash(Run1[i]) * static_cast<uint64>(i + 1);
		}
		TestUtilities::CompareGoldenChecksum(*this, TEXT("EventBus.ReplayConsistency"), BusChecksum);
	}

	return true;
}

// ============================================================================
// Empty Flush Is Safe
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusEmptyFlushTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.EmptyFlush",
	GALAXY_TEST_FLAGS
)

bool FEventBusEmptyFlushTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 Dispatched = Bus.Flush();
	TestEqual(TEXT("Empty flush should return 0"), Dispatched, 0);
	TestTrue(TEXT("Queue should remain empty"), Bus.IsQueueEmpty());

	return true;
}

// ============================================================================
// Reinitialize Clears State
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusReinitializeTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.Reinitialize",
	GALAXY_TEST_FLAGS
)

bool FEventBusReinitializeTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 Count = 0;
	Bus.Subscribe<FTestEventA>([&Count](const FTestEventA&) { Count++; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);

	// Reinitialize should clear everything
	Bus.Initialize(FEventBusConfig{});
	TestTrue(TEXT("Queue should be empty after reinit"), Bus.IsQueueEmpty());
	TestEqual(TEXT("Handler count should be 0"), Bus.GetHandlerCount<FTestEventA>(), 0);

	Bus.Flush();
	TestEqual(TEXT("Handler should not have fired"), Count, 0);

	return true;
}

// ============================================================================
// Telemetry: Counters Tracked Correctly
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusTelemetryCountersTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.TelemetryCounters",
	GALAXY_TEST_FLAGS
)

bool FEventBusTelemetryCountersTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	Bus.Subscribe<FTestEventA>([](const FTestEventA&) {});

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.Flush();

	const FEventBusTelemetry& Telem = Bus.GetTelemetry();
	TestTrue(TEXT("Events dispatched > 0"), Telem.TotalDispatched > 0);

	// Reset telemetry
	FEventBusTelemetry MutableTelem = Telem;
	MutableTelem.Reset();
	TestEqual(TEXT("Reset dispatched = 0"), MutableTelem.TotalDispatched, static_cast<uint64>(0));

	return true;
}

// ============================================================================
// Handler Handle: Validity Check
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusHandleValidityTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.HandleValidity",
	GALAXY_TEST_FLAGS
)

bool FEventBusHandleValidityTest::RunTest(const FString& Parameters)
{
	// Default handle should be invalid
	FEventHandlerHandle DefaultHandle;
	TestFalse(TEXT("Default handle invalid"), DefaultHandle.IsValid());

	// Handle from subscribe should be valid
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	FEventHandlerHandle Handle = Bus.Subscribe<FTestEventA>([](const FTestEventA&) {});
	TestTrue(TEXT("Subscribed handle is valid"), Handle.IsValid());

	// Equality
	FEventHandlerHandle Copy = Handle;
	TestTrue(TEXT("Copied handle equals original"), Handle == Copy);
	TestFalse(TEXT("Default != subscribed"), DefaultHandle == Handle);

	return true;
}

// ============================================================================
// Tick Tracking
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusTickTrackingTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.TickTracking",
	GALAXY_TEST_FLAGS
)

bool FEventBusTickTrackingTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	TestEqual(TEXT("Initial tick = 0"), Bus.GetCurrentTick(), static_cast<uint64>(0));

	Bus.SetCurrentTick(42);
	TestEqual(TEXT("Tick set to 42"), Bus.GetCurrentTick(), static_cast<uint64>(42));

	Bus.SetCurrentTick(43);
	TestEqual(TEXT("Tick advanced to 43"), Bus.GetCurrentTick(), static_cast<uint64>(43));

	return true;
}

// ============================================================================
// ClearQueue: Drops Pending Events
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusClearQueueDropTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.ClearQueueDrop",
	GALAXY_TEST_FLAGS
)

bool FEventBusClearQueueDropTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 Count = 0;
	Bus.Subscribe<FTestEventA>([&Count](const FTestEventA&) { Count++; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	TestFalse(TEXT("Queue not empty before clear"), Bus.IsQueueEmpty());

	const int32 Dropped = Bus.ClearQueue();
	TestEqual(TEXT("2 events dropped"), Dropped, 2);
	TestTrue(TEXT("Queue empty after clear"), Bus.IsQueueEmpty());

	// Flush should dispatch nothing
	Bus.Flush();
	TestEqual(TEXT("No handlers fired"), Count, 0);

	return true;
}

// ============================================================================
// Lambda Subscribe (type-deduced)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusLambdaSubscribeTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.LambdaSubscribe",
	GALAXY_TEST_FLAGS
)

bool FEventBusLambdaSubscribeTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 ValueA = 0;
	FString MessageB;

	Bus.SubscribeLambda([&ValueA](const FTestEventA& E) { ValueA = E.Value; });
	Bus.SubscribeLambda([&MessageB](const FTestEventB& E) { MessageB = E.Message; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 10);
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("Hello"));
	Bus.Flush();

	TestEqual(TEXT("Lambda A received value"), ValueA, 10);
	TestEqual(TEXT("Lambda B received message"), MessageB, FString(TEXT("Hello")));

	return true;
}

// ============================================================================
// Stress: Many Events
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusStressTest,
	"GALAXY_PROJECT.EventBus.EventBusManager.Stress.ManyEvents",
	GALAXY_TEST_FLAGS
)

bool FEventBusStressTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FEventBusManager Bus;
	FEventBusConfig Config;
	Config.MaxQueueDepth = 2000;
	Bus.Initialize(Config);

	int32 Count = 0;
	Bus.Subscribe<FTestEventA>([&Count](const FTestEventA&) { Count++; });

	const int32 NumEvents = 1000;
	for (int32 i = 0; i < NumEvents; ++i)
	{
		Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, i);
	}

	int32 Dispatched;
	{
		TestUtilities::FScopedPerfTimer Timer(*this, TEXT("EventBusFlush1k"));
		Dispatched = Bus.Flush();
	}
	TestEqual(TEXT("1000 events dispatched"), Dispatched, NumEvents);
	TestEqual(TEXT("1000 handler calls"), Count, NumEvents);

	return true;
}

// ============================================================================
// FUNCTIONAL TESTS
// ============================================================================

// ============================================================================
// EventBus Multi-Subscriber Cross-Type Dispatch
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusCrossTypeDispatchTest,
	"GALAXY_PROJECT.EventBus.MultiType.Functional.CrossTypeDispatch",
	GALAXY_TEST_FLAGS
)

bool FEventBusCrossTypeDispatchTest::RunTest(const FString& Parameters)
{
	TestUtilities::FScopedTestIsolation Isolation(*this);
	(void)Parameters;
	// Test that multiple event types interact correctly through dispatch pipeline
	FEventBusManager Bus;
	FEventBusConfig Config;
	Config.bEnableTelemetry = true;
	Bus.Initialize(Config);
	Bus.SetCurrentTick(1);

	// State tracking across event types
	TArray<FString> DispatchLog;

	Bus.Subscribe<FTestEventA>([&DispatchLog](const FTestEventA& E)
	{
		DispatchLog.Add(FString::Printf(TEXT("A:%d"), E.Value));
	}, EEventPriority::Normal);

	Bus.Subscribe<FTestEventB>([&DispatchLog](const FTestEventB& E)
	{
		DispatchLog.Add(FString::Printf(TEXT("B:%s"), *E.Message));
	}, EEventPriority::High);

	// Interleave emissions
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("hello"));
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("world"));

	const int32 Dispatched = Bus.Flush();
	TestEqual(TEXT("4 events dispatched"), Dispatched, 4);

	// Verify all events processed (4 events, each with 1 subscriber = 4 log entries)
	TestEqual(TEXT("4 dispatch log entries"), DispatchLog.Num(), 4);

	// Telemetry should track both types
	const auto& Telemetry = Bus.GetTelemetry();
	TestTrue(TEXT("Telemetry recorded dispatches"), Telemetry.TotalDispatched >= 4);

	// Verify determinism: run same sequence again
	FEventBusManager Bus2;
	Bus2.Initialize(Config);
	Bus2.SetCurrentTick(1);

	TArray<FString> DispatchLog2;
	Bus2.Subscribe<FTestEventA>([&DispatchLog2](const FTestEventA& E)
	{
		DispatchLog2.Add(FString::Printf(TEXT("A:%d"), E.Value));
	}, EEventPriority::Normal);

	Bus2.Subscribe<FTestEventB>([&DispatchLog2](const FTestEventB& E)
	{
		DispatchLog2.Add(FString::Printf(TEXT("B:%s"), *E.Message));
	}, EEventPriority::High);

	Bus2.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus2.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("hello"));
	Bus2.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus2.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("world"));
	Bus2.Flush();

	TestEqual(TEXT("Dispatch logs match"), DispatchLog, DispatchLog2);

	return true;
}

// ============================================================================
// PHASE C TESTS
// ============================================================================

// ============================================================================
// C1: No-subscriber early-out increments DroppedNoSubscribers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusC1NoSubscriberDropTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.C1_NoSubscriberDrop",
	GALAXY_TEST_FLAGS
)

bool FEventBusC1NoSubscriberDropTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	// No subscribers registered -- emit should early-out without queueing
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 99);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 100);

	TestEqual(TEXT("C1: ring should be empty (no handlers)"), Bus.GetQueuedCount(), 0);
	TestTrue(TEXT("C1: queue is empty"), Bus.IsQueueEmpty());
	TestEqual(TEXT("C1: DroppedNoSubscribers == 2"),
		static_cast<int32>(Bus.GetTelemetry().DroppedNoSubscribers), 2);
	TestEqual(TEXT("C1: TotalEmitted == 0 (dropped before metadata)"),
		static_cast<int32>(Bus.GetTelemetry().TotalEmitted), 0);

	// Register a subscriber and verify events now flow through
	int32 Received = 0;
	auto H = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E) { Received = E.Value; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 7);
	TestEqual(TEXT("C1: ring has 1 event now"), Bus.GetQueuedCount(), 1);
	Bus.Flush();
	TestEqual(TEXT("C1: handler fires after subscriber added"), Received, 7);

	Bus.Unsubscribe(H);
	return true;
}

// ============================================================================
// C2: Ring buffer overflow -- DropOldest policy
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusC2RingOverflowDropOldestTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.C2_RingOverflowDropOldest",
	GALAXY_TEST_FLAGS
)

bool FEventBusC2RingOverflowDropOldestTest::RunTest(const FString& Parameters)
{
	// Small ring so overflow is easy to trigger
	FEventBusConfig Cfg;
	Cfg.MaxQueueDepth = 4;
	Cfg.OverflowPolicy = EQueueOverflowPolicy::DropOldest;

	FEventBusManager Bus;
	Bus.Initialize(Cfg);
	Bus.SetCurrentTick(1);

	TArray<int32> Received;
	auto H = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E)
	{
		Received.Add(E.Value);
	});

	// Fill to capacity: values 1..4
	for (int32 i = 1; i <= 4; ++i)
		Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, i);

	TestEqual(TEXT("C2: ring full at 4"), Bus.GetQueuedCount(), 4);
	TestEqual(TEXT("C2: no overflow yet"), static_cast<int32>(Bus.GetTelemetry().DroppedOverflow), 0);

	// 5th event should drop oldest (value 1)
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 5);
	TestEqual(TEXT("C2: ring still 4 after overflow"), Bus.GetQueuedCount(), 4);
	TestEqual(TEXT("C2: DroppedOverflow == 1"), static_cast<int32>(Bus.GetTelemetry().DroppedOverflow), 1);

	Bus.Flush();
	TestEqual(TEXT("C2: 4 events dispatched"), Received.Num(), 4);
	// Events 2,3,4,5 should be received (oldest=1 was dropped)
	TestEqual(TEXT("C2: oldest (1) dropped; first received is 2"), Received[0], 2);
	TestEqual(TEXT("C2: last received is 5"), Received[3], 5);

	Bus.Unsubscribe(H);
	return true;
}

// ============================================================================
// C2: Ring buffer overflow -- DropNewest policy
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusC2RingOverflowDropNewestTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.C2_RingOverflowDropNewest",
	GALAXY_TEST_FLAGS
)

bool FEventBusC2RingOverflowDropNewestTest::RunTest(const FString& Parameters)
{
	FEventBusConfig Cfg;
	Cfg.MaxQueueDepth = 3;
	Cfg.OverflowPolicy = EQueueOverflowPolicy::DropNewest;

	FEventBusManager Bus;
	Bus.Initialize(Cfg);
	Bus.SetCurrentTick(1);

	TArray<int32> Received;
	auto H = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E)
	{
		Received.Add(E.Value);
	});

	for (int32 i = 1; i <= 5; ++i)
		Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, i);

	// Only 3 slots; events 4 and 5 should be dropped (DropNewest)
	TestEqual(TEXT("C2: ring holds 3"), Bus.GetQueuedCount(), 3);
	TestEqual(TEXT("C2: DroppedOverflow == 2"), static_cast<int32>(Bus.GetTelemetry().DroppedOverflow), 2);

	Bus.Flush();
	TestEqual(TEXT("C2: 3 events dispatched"), Received.Num(), 3);
	TestEqual(TEXT("C2: first received is 1 (oldest kept)"), Received[0], 1);
	TestEqual(TEXT("C2: last received is 3"), Received[2], 3);

	Bus.Unsubscribe(H);
	return true;
}

// ============================================================================
// C3: Coalescing -- same key produces one dispatch with latest payload
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusC3CoalesceBasicTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.C3_CoalesceBasic",
	GALAXY_TEST_FLAGS
)

bool FEventBusC3CoalesceBasicTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	TArray<int32> Received;
	auto H = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E)
	{
		Received.Add(E.Value);
	});

	const uint64 Key = 42;
	Bus.EmitCoalesce<FTestEventA>(Key, NAME_None, 10);
	Bus.EmitCoalesce<FTestEventA>(Key, NAME_None, 20);
	Bus.EmitCoalesce<FTestEventA>(Key, NAME_None, 30); // last-write wins

	// No ring events; ring should be empty
	TestTrue(TEXT("C3: ring is empty (coalesced, not queued)"), Bus.IsQueueEmpty());

	const int32 Dispatched = Bus.Flush();
	TestEqual(TEXT("C3: 1 coalesced event dispatched"), Dispatched, 1);
	TestEqual(TEXT("C3: handler called once"), Received.Num(), 1);
	TestEqual(TEXT("C3: last-write payload wins (30)"), Received[0], 30);

	Bus.Unsubscribe(H);
	return true;
}

// ============================================================================
// C3: Coalescing -- different keys produce independent dispatches
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusC3CoalesceMultiKeyTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.C3_CoalesceMultiKey",
	GALAXY_TEST_FLAGS
)

bool FEventBusC3CoalesceMultiKeyTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	TArray<int32> Received;
	auto H = Bus.Subscribe<FTestEventA>([&Received](const FTestEventA& E)
	{
		Received.Add(E.Value);
	});

	// Three distinct keys
	Bus.EmitCoalesce<FTestEventA>(1, NAME_None, 100);
	Bus.EmitCoalesce<FTestEventA>(2, NAME_None, 200);
	Bus.EmitCoalesce<FTestEventA>(3, NAME_None, 300);
	// Overwrite key 1 with new value
	Bus.EmitCoalesce<FTestEventA>(1, NAME_None, 111);

	const int32 Dispatched = Bus.Flush();
	TestEqual(TEXT("C3: 3 distinct coalesce keys = 3 dispatches"), Dispatched, 3);
	TestEqual(TEXT("C3: 3 handler calls"), Received.Num(), 3);

	// Dispatched in TypeId+Key sorted order; all keys same type so sorted by key: 1, 2, 3
	TestEqual(TEXT("C3: key 1 value is 111 (overwritten)"), Received[0], 111);
	TestEqual(TEXT("C3: key 2 value is 200"), Received[1], 200);
	TestEqual(TEXT("C3: key 3 value is 300"), Received[2], 300);

	Bus.Unsubscribe(H);
	return true;
}

// ============================================================================
// C3: Coalescing -- coalesced events dispatch before ring queue events
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusC3CoalesceOrderBeforeRingTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.C3_CoalesceOrderBeforeRing",
	GALAXY_TEST_FLAGS
)

bool FEventBusC3CoalesceOrderBeforeRingTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	TArray<FString> Log;
	auto H = Bus.Subscribe<FTestEventA>([&Log](const FTestEventA& E)
	{
		Log.Add(FString::Printf(TEXT("ring:%d"), E.Value));
	});
	auto H2 = Bus.Subscribe<FTestEventB>([&Log](const FTestEventB& E)
	{
		Log.Add(FString::Printf(TEXT("coalesce:%s"), *E.Message));
	});

	// One ring event, one coalesced event
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitCoalesce<FTestEventB>(77, NAME_None, TEXT("hello"));

	Bus.Flush();

	TestEqual(TEXT("C3: 2 total dispatches"), Log.Num(), 2);
	// Coalesced dispatches before ring
	TestEqual(TEXT("C3: coalesced event fires first"), Log[0], TEXT("coalesce:hello"));
	TestEqual(TEXT("C3: ring event fires second"), Log[1], TEXT("ring:1"));

	Bus.Unsubscribe(H);
	Bus.Unsubscribe(H2);
	return true;
}

// ============================================================================
// C1 + C2: No-subscriber drop still applies with small ring
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusC1C2CombinedTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.C1C2_Combined",
	GALAXY_TEST_FLAGS
)

bool FEventBusC1C2CombinedTest::RunTest(const FString& Parameters)
{
	FEventBusConfig Cfg;
	Cfg.MaxQueueDepth = 2;
	FEventBusManager Bus;
	Bus.Initialize(Cfg);
	Bus.SetCurrentTick(1);

	// No subscriber -- 5 emits all hit C1 early-out, nothing hits the ring
	for (int32 i = 0; i < 5; ++i)
		Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, i);

	TestEqual(TEXT("C1+C2: ring empty"), Bus.GetQueuedCount(), 0);
	TestEqual(TEXT("C1+C2: DroppedNoSubscribers == 5"),
		static_cast<int32>(Bus.GetTelemetry().DroppedNoSubscribers), 5);
	TestEqual(TEXT("C1+C2: DroppedOverflow == 0"),
		static_cast<int32>(Bus.GetTelemetry().DroppedOverflow), 0);

	return true;
}

// PHASE D TESTS

namespace
{
	/** Minimal IEventRecorder implementation for unit tests. */
	class FMockEventRecorder final : public IEventRecorder
	{
	public:
		int32 EmitCount    = 0;
		int32 DispatchCount = 0;
		FString LastEmittedEventName;
		FString LastDispatchedEventName;

		void RecordEmit(const IEvent& Event, const FEventMetadata&) override
		{
			++EmitCount;
			LastEmittedEventName = Event.GetEventName();
		}

		void RecordDispatch(const IEvent& Event, uint64) override
		{
			++DispatchCount;
			LastDispatchedEventName = Event.GetEventName();
		}
	};
}

// ============================================================================
// D1: Recorder RecordEmit and RecordDispatch are called on emit + flush
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusD1RecorderHooksTest,
	"GALAXY_PROJECT.EventBus.Observability.Unit.D1_RecorderHooksInvoked",
	GALAXY_TEST_FLAGS
)

bool FEventBusD1RecorderHooksTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	FMockEventRecorder Recorder;
	Bus.SetEventRecorder(&Recorder);

	// Subscribe so the C1 early-out does not suppress the event
	int32 HandlerCallCount = 0;
	Bus.Subscribe<FTestEventA>([&HandlerCallCount](const FTestEventA&) { ++HandlerCallCount; });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 42);

	// RecordEmit fires at emit time (before Flush)
	TestEqual(TEXT("D1: RecordEmit called at Emit()"), Recorder.EmitCount, 1);
	TestEqual(TEXT("D1: EmitCount before flush"), Recorder.DispatchCount, 0);
	TestEqual(TEXT("D1: LastEmitted is TestEventA"), Recorder.LastEmittedEventName, TEXT("TestEventA"));

	Bus.Flush();

	// RecordDispatch fires once per handler invocation during Flush
	TestEqual(TEXT("D1: handler called"), HandlerCallCount, 1);
	TestEqual(TEXT("D1: RecordDispatch called once"), Recorder.DispatchCount, 1);
	TestEqual(TEXT("D1: LastDispatched is TestEventA"), Recorder.LastDispatchedEventName, TEXT("TestEventA"));

	// Emit a second event -- recorder should accumulate
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 7);
	Bus.Flush();
	TestEqual(TEXT("D1: EmitCount accumulates"), Recorder.EmitCount, 2);
	TestEqual(TEXT("D1: DispatchCount accumulates"), Recorder.DispatchCount, 2);

	// Detach recorder; further emits should not reach it
	Bus.SetEventRecorder(nullptr);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 99);
	Bus.Flush();
	TestEqual(TEXT("D1: no emit after detach"), Recorder.EmitCount, 2);
	TestEqual(TEXT("D1: no dispatch after detach"), Recorder.DispatchCount, 2);

	return true;
}

// ============================================================================
// D2: Per-domain telemetry tracks emits and dispatches by source domain
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusD2DomainTelemetryTest,
	"GALAXY_PROJECT.EventBus.Observability.Unit.D2_DomainTelemetry",
	GALAXY_TEST_FLAGS
)

bool FEventBusD2DomainTelemetryTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	// Subscribers needed to bypass C1 early-out
	int32 ACount = 0;
	int32 BCount = 0;
	Bus.Subscribe<FTestEventA>([&ACount](const FTestEventA&) { ++ACount; });
	Bus.Subscribe<FTestEventB>([&BCount](const FTestEventB&) { ++BCount; });

	const FName DomainAlpha = TEXT("DomainAlpha");
	const FName DomainBeta  = TEXT("DomainBeta");

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, DomainAlpha, 1);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, DomainAlpha, 2);
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, DomainBeta, TEXT("hi"));
	// NAME_None emission -- must NOT create an entry in DomainTelemetry
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 3);

	Bus.Flush();

	const TMap<FName, FDomainStats>& DT = Bus.GetDomainTelemetry();
	TestEqual(TEXT("D2: exactly 2 domain entries"), DT.Num(), 2);

	const FDomainStats* Alpha = DT.Find(DomainAlpha);
	TestTrue(TEXT("D2: DomainAlpha entry exists"), Alpha != nullptr);
	if (Alpha)
	{
		TestEqual(TEXT("D2: Alpha TotalEmitted == 2"),
			static_cast<int32>(Alpha->TotalEmitted), 2);
		TestEqual(TEXT("D2: Alpha TotalDispatched == 2"),
			static_cast<int32>(Alpha->TotalDispatched), 2);
		TestEqual(TEXT("D2: Alpha TotalHandlerCalls == 2"),
			static_cast<int32>(Alpha->TotalHandlerCalls), 2);
	}

	const FDomainStats* Beta = DT.Find(DomainBeta);
	TestTrue(TEXT("D2: DomainBeta entry exists"), Beta != nullptr);
	if (Beta)
	{
		TestEqual(TEXT("D2: Beta TotalEmitted == 1"),
			static_cast<int32>(Beta->TotalEmitted), 1);
		TestEqual(TEXT("D2: Beta TotalDispatched == 1"),
			static_cast<int32>(Beta->TotalDispatched), 1);
		TestEqual(TEXT("D2: Beta TotalHandlerCalls == 1"),
			static_cast<int32>(Beta->TotalHandlerCalls), 1);
	}

	// Aggregate telemetry must match: 4 emitted (3 handled + 1 no-domain)
	// Note: NAME_None is counted in aggregate but not in DomainTelemetry
	TestEqual(TEXT("D2: aggregate TotalEmitted == 4"),
		static_cast<int32>(Bus.GetTelemetry().TotalEmitted), 4);

	return true;
}

// ============================================================================
// D3: OnPreFlush and OnPostFlush fire in correct order relative to handlers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusD3FlushHooksOrderTest,
	"GALAXY_PROJECT.EventBus.Observability.Unit.D3_PrePostFlushHooksOrder",
	GALAXY_TEST_FLAGS
)

bool FEventBusD3FlushHooksOrderTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	TArray<int32> FireOrder;

	// 1 = pre-flush, 2 = event handler, 3 = post-flush
	Bus.OnPreFlush.AddLambda([&FireOrder](FEventBusManager&) { FireOrder.Add(1); });
	Bus.Subscribe<FTestEventA>([&FireOrder](const FTestEventA&) { FireOrder.Add(2); });
	Bus.OnPostFlush.AddLambda([&FireOrder](FEventBusManager&) { FireOrder.Add(3); });

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 0);
	Bus.Flush();

	TestEqual(TEXT("D3: exactly 3 calls"), FireOrder.Num(), 3);
	TestEqual(TEXT("D3: pre-flush fires first"), FireOrder[0], 1);
	TestEqual(TEXT("D3: event handler fires second"), FireOrder[1], 2);
	TestEqual(TEXT("D3: post-flush fires last"), FireOrder[2], 3);

	// Hooks survive across multiple flushes
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 0);
	Bus.Flush();
	TestEqual(TEXT("D3: 6 calls after second flush"), FireOrder.Num(), 6);

	// Flush with no events: pre + post still fire, no handler
	FireOrder.Empty();
	Bus.Flush();
	TestEqual(TEXT("D3: empty flush fires pre + post"), FireOrder.Num(), 2);
	TestEqual(TEXT("D3: pre fires on empty flush"), FireOrder[0], 1);
	TestEqual(TEXT("D3: post fires on empty flush"), FireOrder[1], 3);

	return true;
}

// PHASE A TESTS

// ============================================================================
// A1: Stable type IDs -- two distinct event types must not share a type ID,
//     and the same type always returns the same ID within a process.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusA1StableTypeIdsTest,
	"GALAXY_PROJECT.EventBus.Determinism.Unit.A1_StableTypeIds",
	GALAXY_TEST_FLAGS
)

bool FEventBusA1StableTypeIdsTest::RunTest(const FString& Parameters)
{
	// IDs are compile-time constants -- calling twice must return the same value.
	const uint64 IdA1 = FTestEventA::GetStaticTypeId();
	const uint64 IdA2 = FTestEventA::GetStaticTypeId();
	TestEqual(TEXT("A1: same type returns same ID"), IdA1, IdA2);

	// Two different types must not collide.
	const uint64 IdB = FTestEventB::GetStaticTypeId();
	TestNotEqual(TEXT("A1: FTestEventA and FTestEventB have different IDs"), IdA1, IdB);

	const uint64 IdEntity = FTestEntityEvent::GetStaticTypeId();
	TestNotEqual(TEXT("A1: FTestEventA and FTestEntityEvent have different IDs"), IdA1, IdEntity);
	TestNotEqual(TEXT("A1: FTestEventB and FTestEntityEvent have different IDs"), IdB, IdEntity);

	// IDs are non-zero (zero is the sentinel for an unset TypeId in FEventHandlerHandle).
	TestNotEqual(TEXT("A1: FTestEventA ID is non-zero"), IdA1, static_cast<uint64>(0));
	TestNotEqual(TEXT("A1: FTestEventB ID is non-zero"), IdB, static_cast<uint64>(0));

	// The runtime GetTypeId() on an instance must match the static compile-time value.
	FTestEventA InstanceA;
	TestEqual(TEXT("A1: instance GetTypeId() matches static"), InstanceA.GetTypeId(), IdA1);

	return true;
}

// ============================================================================
// A3: Tombstone dispatch -- a handler that unsubscribes itself mid-dispatch
//     must not fire again in the same flush, and subsequent emits work cleanly.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusA3TombstoneSafetyTest,
	"GALAXY_PROJECT.EventBus.Determinism.Unit.A3_TombstoneSafety",
	GALAXY_TEST_FLAGS
)

bool FEventBusA3TombstoneSafetyTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	int32 SelfUnsub = 0;
	int32 OtherCount = 0;
	FEventHandlerHandle SelfHandle;

	// Handler A: unsubscribes itself during dispatch
	SelfHandle = Bus.Subscribe<FTestEventA>([&](const FTestEventA&)
	{
		++SelfUnsub;
		Bus.Unsubscribe(SelfHandle);
	});

	// Handler B: always fires (registered after A, lower priority = later in order)
	Bus.Subscribe<FTestEventA>([&OtherCount](const FTestEventA&)
	{
		++OtherCount;
	}, EEventPriority::Low);

	// First flush: both A and B should fire exactly once
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.Flush();
	TestEqual(TEXT("A3: self-unsubscribing handler fired once"), SelfUnsub, 1);
	TestEqual(TEXT("A3: other handler fired once on first flush"), OtherCount, 1);

	// Second flush: self-unsubscribed handler must NOT fire again
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.Flush();
	TestEqual(TEXT("A3: self-unsubscribing handler did not fire again"), SelfUnsub, 1);
	TestEqual(TEXT("A3: other handler fired on second flush too"), OtherCount, 2);

	return true;
}

// ============================================================================
// A2: Re-entrant emission warning -- with bAllowReentrantEmission=false, a
//     handler that emits during Flush() increments ReentrantEmissions telemetry.
//     The re-entrant event lands in the NEXT flush (strict 1-tick latency).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusA2ReentrantPolicyTest,
	"GALAXY_PROJECT.EventBus.Determinism.Unit.A2_ReentrantEmissionPolicy",
	GALAXY_TEST_FLAGS
)

bool FEventBusA2ReentrantPolicyTest::RunTest(const FString& Parameters)
{
	FEventBusConfig Cfg;
	Cfg.bAllowReentrantEmission = false; // warning mode; behavior is identical
	FEventBusManager Bus;
	Bus.Initialize(Cfg);
	Bus.SetCurrentTick(1);

	int32 FirstCount  = 0;
	int32 SecondCount = 0;

	// First handler: emits a second event during dispatch
	Bus.Subscribe<FTestEventA>([&](const FTestEventA& E)
	{
		++FirstCount;
		if (E.Value == 1)
		{
			// Re-entrant emit -- must land in the NEXT flush
			Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
		}
	});

	// Second handler: counts Value==2 arrivals
	Bus.Subscribe<FTestEventA>([&](const FTestEventA& E)
	{
		if (E.Value == 2)
			++SecondCount;
	});

	// Emit the seed event and flush
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.Flush();

	// After first flush: seed fired, re-entrant queued but NOT yet dispatched
	TestEqual(TEXT("A2: first handler fired once on flush 1"), FirstCount, 1);
	TestEqual(TEXT("A2: re-entrant NOT dispatched in same flush"), SecondCount, 0);
	TestEqual(TEXT("A2: ReentrantEmissions counter incremented"),
		static_cast<int32>(Bus.GetTelemetry().ReentrantEmissions), 1);
	TestEqual(TEXT("A2: re-entrant event queued for next flush"), Bus.GetQueuedCount(), 1);

	// Second flush dispatches the re-entrant event
	Bus.Flush();
	TestEqual(TEXT("A2: re-entrant event dispatched in flush 2"), SecondCount, 1);
	TestTrue(TEXT("A2: queue empty after second flush"), Bus.IsQueueEmpty());

	return true;
}

// ============================================================================
// A4: Serialize round-trip -- CurrentTick and TickSequence survive save/load.
//     Queue must be empty before serializing (assert contract check via clean state).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusA4SerializeRoundTripTest,
	"GALAXY_PROJECT.EventBus.Determinism.Unit.A4_SerializeRoundTrip",
	GALAXY_TEST_FLAGS
)

bool FEventBusA4SerializeRoundTripTest::RunTest(const FString& Parameters)
{
	FEventBusManager Source;
	Source.Initialize(FEventBusConfig{});
	Source.SetCurrentTick(9999);

	// Advance TickSequence by emitting (and flushing to keep queue clean for serialize).
	// Subscribe so C1 early-out doesn't suppress the emits.
	Source.Subscribe<FTestEventA>([](const FTestEventA&) {});
	Source.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Source.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Source.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 3);
	Source.Flush(); // drains queue; TickSequence is now 3

	TestTrue(TEXT("A4: queue empty before serialize"), Source.IsQueueEmpty());
	TestEqual(TEXT("A4: CurrentTick is 9999"), static_cast<int32>(Source.GetCurrentTick()), 9999);

	// Serialize to memory
	TArray<uint8> Buffer;
	{
		FMemoryWriter Writer(Buffer);
		Source.Serialize(Writer);
	}
	TestTrue(TEXT("A4: serialized buffer is non-empty"), Buffer.Num() > 0);

	// Deserialize into a fresh bus
	FEventBusManager Target;
	Target.Initialize(FEventBusConfig{});
	{
		FMemoryReader Reader(Buffer);
		Target.Serialize(Reader);
	}

	TestEqual(TEXT("A4: CurrentTick restored"),
		Target.GetCurrentTick(), Source.GetCurrentTick());

	return true;
}

// ============================================================================
// Coalesce overflow cap (item 3)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusCoalesceOverflowCapTest,
	"GALAXY_PROJECT.EventBus.Performance.Unit.CoalesceOverflowCap",
	GALAXY_TEST_FLAGS
)

bool FEventBusCoalesceOverflowCapTest::RunTest(const FString& Parameters)
{
	FEventBusConfig Cfg;
	Cfg.MaxCoalesceSlots = 3; // very small cap
	FEventBusManager Bus;
	Bus.Initialize(Cfg);
	Bus.SetCurrentTick(1);

	int32 HandlerCalls = 0;
	auto H = Bus.Subscribe<FTestEventA>([&HandlerCalls](const FTestEventA&) { ++HandlerCalls; });

	// Fill to capacity with 3 distinct keys
	Bus.EmitCoalesce<FTestEventA>(/*Key*/ 1, NAME_None, 1);
	Bus.EmitCoalesce<FTestEventA>(/*Key*/ 2, NAME_None, 2);
	Bus.EmitCoalesce<FTestEventA>(/*Key*/ 3, NAME_None, 3);
	TestEqual(TEXT("Cap: overflow counter still zero"),
		static_cast<int32>(Bus.GetTelemetry().DroppedCoalesceOverflow), 0);

	// Fourth distinct key -> dropped
	Bus.EmitCoalesce<FTestEventA>(/*Key*/ 4, NAME_None, 4);
	TestEqual(TEXT("Cap: 4th distinct key dropped"),
		static_cast<int32>(Bus.GetTelemetry().DroppedCoalesceOverflow), 1);

	// Re-emit for an existing key MUST succeed (last-write-wins, no cap check)
	Bus.EmitCoalesce<FTestEventA>(/*Key*/ 2, NAME_None, 222);
	TestEqual(TEXT("Cap: re-emit for existing key not counted"),
		static_cast<int32>(Bus.GetTelemetry().DroppedCoalesceOverflow), 1);

	// Flush dispatches exactly 3 (the cap)
	Bus.Flush();
	TestEqual(TEXT("Cap: exactly 3 coalesced events dispatched"), HandlerCalls, 3);

	Bus.Unsubscribe(H);
	return true;
}

// ============================================================================
// UnsubscribeByOwner (item 4)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusUnsubscribeByOwnerTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.UnsubscribeByOwner",
	GALAXY_TEST_FLAGS
)

bool FEventBusUnsubscribeByOwnerTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	// Use permanent engine objects as dummy owners -- always alive, no GC concerns,
	// and UObject::StaticClass() is abstract in UE5.5 so direct NewObject<UObject>
	// hits an ensure.  GetTransientPackage() and GEngine are valid non-null UObject*
	// singletons suitable as weak-subscription lifetime guards.
	UObject* OwnerA = GetTransientPackage(); // never null, never GC'd
	UObject* OwnerB = GEngine;              // never null in editor/cmd builds

	int32 CountA = 0;
	int32 CountB = 0;

	// 2 handlers on A (one per event type), 1 on B
	Bus.SubscribeWeak<FTestEventA>(OwnerA, [&CountA](const FTestEventA&) { ++CountA; });
	Bus.SubscribeWeak<FTestEventB>(OwnerA, [&CountA](const FTestEventB&) { ++CountA; });
	Bus.SubscribeWeak<FTestEventA>(OwnerB, [&CountB](const FTestEventA&) { ++CountB; });

	TestEqual(TEXT("UbO: 3 handlers before"),
		Bus.GetTelemetry().SubscriptionCount, 3);

	const int32 RemovedA = Bus.UnsubscribeByOwner(OwnerA);
	TestEqual(TEXT("UbO: removed both OwnerA handlers"), RemovedA, 2);
	TestEqual(TEXT("UbO: 1 handler remaining"),
		Bus.GetTelemetry().SubscriptionCount, 1);

	// Emit and flush -- only OwnerB should fire
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("x"));
	Bus.Flush();

	TestEqual(TEXT("UbO: OwnerA handlers did not fire"), CountA, 0);
	TestEqual(TEXT("UbO: OwnerB handler fired once"), CountB, 1);

	// Calling again returns 0
	TestEqual(TEXT("UbO: second call returns 0"), Bus.UnsubscribeByOwner(OwnerA), 0);

	// Null owner -> 0
	TestEqual(TEXT("UbO: null owner returns 0"), Bus.UnsubscribeByOwner(nullptr), 0);

	return true;
}

// ============================================================================
// Per-type telemetry (item 5) + byte accounting (item 7)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusTypeTelemetryTest,
	"GALAXY_PROJECT.EventBus.Observability.Unit.TypeTelemetry",
	GALAXY_TEST_FLAGS
)

bool FEventBusTypeTelemetryTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});
	Bus.SetCurrentTick(1);

	auto HA = Bus.Subscribe<FTestEventA>([](const FTestEventA&) {});
	auto HB = Bus.Subscribe<FTestEventB>([](const FTestEventB&) {});

	// Emit 3 of A, 2 of B
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 3);
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("x"));
	Bus.EmitNew<FTestEventB>(EEventDispatchMode::Queued, NAME_None, TEXT("y"));
	Bus.Flush();

	const auto& ByType = Bus.GetTypeTelemetry();
	TestEqual(TEXT("Type: two distinct types tracked"), ByType.Num(), 2);

	const FTypeStats* StatsA = ByType.Find(FTestEventA::GetStaticTypeId());
	const FTypeStats* StatsB = ByType.Find(FTestEventB::GetStaticTypeId());
	TestNotNull(TEXT("Type: stats for A exist"), StatsA);
	TestNotNull(TEXT("Type: stats for B exist"), StatsB);
	if (!StatsA || !StatsB) return false;

	TestEqual(TEXT("Type: A name captured"), StatsA->EventName, FString(TEXT("TestEventA")));
	TestEqual(TEXT("Type: B name captured"), StatsB->EventName, FString(TEXT("TestEventB")));

	TestEqual(TEXT("Type: A emitted=3"), static_cast<int32>(StatsA->TotalEmitted), 3);
	TestEqual(TEXT("Type: A dispatched=3"), static_cast<int32>(StatsA->TotalDispatched), 3);
	TestEqual(TEXT("Type: A handler_calls=3"), static_cast<int32>(StatsA->TotalHandlerCalls), 3);

	TestEqual(TEXT("Type: B emitted=2"), static_cast<int32>(StatsB->TotalEmitted), 2);
	TestEqual(TEXT("Type: B dispatched=2"), static_cast<int32>(StatsB->TotalDispatched), 2);
	TestEqual(TEXT("Type: B handler_calls=2"), static_cast<int32>(StatsB->TotalHandlerCalls), 2);

	// Byte accounting: per-type bytes must sum to aggregate TotalBytesEmitted
	const uint64 SumBytes = StatsA->BytesEmitted + StatsB->BytesEmitted;
	TestEqual(TEXT("Bytes: per-type sum == aggregate"),
		SumBytes, Bus.GetTelemetry().TotalBytesEmitted);

	// Per-type bytes must equal count * sizeof(DerivedEventType)
	TestEqual(TEXT("Bytes: A = 3 * sizeof(FTestEventA)"),
		StatsA->BytesEmitted, static_cast<uint64>(3 * sizeof(FTestEventA)));
	TestEqual(TEXT("Bytes: B = 2 * sizeof(FTestEventB)"),
		StatsB->BytesEmitted, static_cast<uint64>(2 * sizeof(FTestEventB)));

	Bus.Unsubscribe(HA);
	Bus.Unsubscribe(HB);
	return true;
}

// ============================================================================
// Deferred-add: a handler that subscribes MANY handlers to the SAME type
// mid-dispatch must not invalidate the executing handler (array realloc UB
// pre-fix). Deferred entries do not fire in the current flush, fire in the
// next one, and are counted by GetHandlerCount immediately.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusSubscribeSameTypeDuringDispatchTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.SubscribeSameTypeDuringDispatchDeferred",
	GALAXY_TEST_FLAGS
)

bool FEventBusSubscribeSameTypeDuringDispatchTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	// Enough mid-dispatch subscribes to force several array growths pre-fix
	constexpr int32 MidDispatchSubscribes = 64;

	int32 SeedHandlerCalls = 0;
	int32 NewHandlerCalls = 0;

	Bus.Subscribe<FTestEventA>([&](const FTestEventA&)
	{
		++SeedHandlerCalls;
		if (SeedHandlerCalls == 1)
		{
			for (int32 i = 0; i < MidDispatchSubscribes; ++i)
			{
				Bus.Subscribe<FTestEventA>([&NewHandlerCalls](const FTestEventA&)
				{
					++NewHandlerCalls;
				});
			}
		}
	});

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.Flush();

	TestEqual(TEXT("Seed handler fired once"), SeedHandlerCalls, 1);
	TestEqual(TEXT("Mid-dispatch subscribers did NOT fire in the same flush"), NewHandlerCalls, 0);
	TestEqual(TEXT("All handlers counted immediately (1 seed + deferred)"),
		Bus.GetHandlerCount<FTestEventA>(), 1 + MidDispatchSubscribes);

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.Flush();

	TestEqual(TEXT("Seed handler fired again on flush 2"), SeedHandlerCalls, 2);
	TestEqual(TEXT("Deferred subscribers all fired on flush 2"), NewHandlerCalls, MidDispatchSubscribes);

	return true;
}

// ============================================================================
// Deferred-add: a handler subscribed mid-dispatch can be unsubscribed in the
// same dispatch (while still pending) -- it must never fire, and the handle
// removal must report success.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEventBusUnsubscribePendingHandlerTest,
	"GALAXY_PROJECT.EventBus.Core.Unit.UnsubscribePendingHandler",
	GALAXY_TEST_FLAGS
)

bool FEventBusUnsubscribePendingHandlerTest::RunTest(const FString& Parameters)
{
	FEventBusManager Bus;
	Bus.Initialize(FEventBusConfig{});

	int32 PendingHandlerCalls = 0;
	bool bUnsubscribeReportedSuccess = false;

	Bus.Subscribe<FTestEventA>([&](const FTestEventA&)
	{
		const FEventHandlerHandle Pending = Bus.Subscribe<FTestEventA>(
			[&PendingHandlerCalls](const FTestEventA&)
			{
				++PendingHandlerCalls;
			});
		bUnsubscribeReportedSuccess = Bus.Unsubscribe(Pending);
	});

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 1);
	Bus.Flush();

	TestTrue(TEXT("Unsubscribe of a pending mid-dispatch handle succeeded"), bUnsubscribeReportedSuccess);
	TestEqual(TEXT("Only the seed handler remains"), Bus.GetHandlerCount<FTestEventA>(), 1);

	Bus.EmitNew<FTestEventA>(EEventDispatchMode::Queued, NAME_None, 2);
	Bus.Flush();

	TestEqual(TEXT("Cancelled pending handler never fired"), PendingHandlerCalls, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS


