package store

import (
	"fmt"

	"github.com/sourcegraph/sourcegraph/internal/metrics"
	"github.com/sourcegraph/sourcegraph/internal/observation"
)

type operations struct {
	vacuumStaleDefinitions           *observation.Operation
	vacuumStaleReferences            *observation.Operation
	vacuumStalePaths                 *observation.Operation
	vacuumAbandonedDefinitions       *observation.Operation
	vacuumAbandonedReferences        *observation.Operation
	vacuumAbandonedInitialPathCounts *observation.Operation
	vacuumStaleGraphs                *observation.Operation
	vacuumStaleRanks                 *observation.Operation
	insertDefinitionsForRanking      *observation.Operation
	insertReferencesForRanking       *observation.Operation
	insertPathCountInputs            *observation.Operation
	insertPathRanks                  *observation.Operation
	insertInitialPathCounts          *observation.Operation
	insertInitialPathRanks           *observation.Operation
}

var m = new(metrics.SingletonREDMetrics)

func newOperations(observationCtx *observation.Context) *operations {
	m := m.Get(func() *metrics.REDMetrics {
		return metrics.NewREDMetrics(
			observationCtx.Registerer,
			"codeintel_ranking_store",
			metrics.WithLabels("op"),
			metrics.WithCountHelp("Total number of method invocations."),
		)
	})

	op := func(name string) *observation.Operation {
		return observationCtx.Operation(observation.Op{
			Name:              fmt.Sprintf("codeintel.ranking.store.%s", name),
			MetricLabelValues: []string{name},
			Metrics:           m,
		})
	}

	return &operations{
		vacuumStaleDefinitions:           op("VacuumStaleDefinitions"),
		vacuumStaleReferences:            op("VacuumStaleReferences"),
		vacuumStalePaths:                 op("VacuumStalePaths"),
		vacuumAbandonedDefinitions:       op("VacuumAbandonedDefinitions"),
		vacuumAbandonedReferences:        op("VacuumAbandonedReferences"),
		vacuumAbandonedInitialPathCounts: op("VacuumAbandonedInitialPathCounts"),
		vacuumStaleGraphs:                op("VacuumStaleGraphs"),
		vacuumStaleRanks:                 op("VacuumStaleRanks"),
		insertDefinitionsForRanking:      op("InsertDefinitionsForRanking"),
		insertReferencesForRanking:       op("InsertReferencesForRanking"),
		insertPathCountInputs:            op("InsertPathCountInputs"),
		insertPathRanks:                  op("InsertPathRanks"),
		insertInitialPathCounts:          op("InsertInitialPathCounts"),
		insertInitialPathRanks:           op("InsertInitialPathRanks"),
	}
}
