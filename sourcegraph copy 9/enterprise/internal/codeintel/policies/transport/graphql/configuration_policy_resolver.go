package graphql

import (
	"context"

	"github.com/graph-gophers/graphql-go"
	"github.com/opentracing/opentracing-go/log"

	sharedresolvers "github.com/sourcegraph/sourcegraph/enterprise/internal/codeintel/shared/resolvers"
	"github.com/sourcegraph/sourcegraph/enterprise/internal/codeintel/shared/types"
	resolverstubs "github.com/sourcegraph/sourcegraph/internal/codeintel/resolvers"
	"github.com/sourcegraph/sourcegraph/internal/database"
	"github.com/sourcegraph/sourcegraph/internal/observation"
	"github.com/sourcegraph/sourcegraph/lib/errors"
)

type configurationPolicyResolver struct {
	repoStore           database.RepoStore
	configurationPolicy types.ConfigurationPolicy
	errTracer           *observation.ErrCollector
}

func NewConfigurationPolicyResolver(repoStore database.RepoStore, configurationPolicy types.ConfigurationPolicy, errTracer *observation.ErrCollector) resolverstubs.CodeIntelligenceConfigurationPolicyResolver {
	return &configurationPolicyResolver{
		repoStore:           repoStore,
		configurationPolicy: configurationPolicy,
		errTracer:           errTracer,
	}
}

func (r *configurationPolicyResolver) ID() graphql.ID {
	return resolverstubs.MarshalID("CodeIntelligenceConfigurationPolicy", r.configurationPolicy.ID)
}

func (r *configurationPolicyResolver) Name() string {
	return r.configurationPolicy.Name
}

func (r *configurationPolicyResolver) Repository(ctx context.Context) (_ resolverstubs.RepositoryResolver, err error) {
	if r.configurationPolicy.RepositoryID == nil {
		return nil, nil
	}

	defer r.errTracer.Collect(&err,
		log.String("configurationPolicyResolver.field", "repository"),
		log.Int("configurationPolicyID", r.configurationPolicy.ID),
		log.Int("repoID", *r.configurationPolicy.RepositoryID),
	)

	return sharedresolvers.NewRepositoryFromID(ctx, r.repoStore, *r.configurationPolicy.RepositoryID)
}

func (r *configurationPolicyResolver) RepositoryPatterns() *[]string {
	return r.configurationPolicy.RepositoryPatterns
}

func (r *configurationPolicyResolver) Type() (_ resolverstubs.GitObjectType, err error) {
	defer r.errTracer.Collect(&err,
		log.String("configurationPolicyResolver.field", "type"),
		log.Int("configurationPolicyID", r.configurationPolicy.ID),
		log.String("policyType", string(r.configurationPolicy.Type)),
	)

	switch r.configurationPolicy.Type {
	case types.GitObjectTypeCommit:
		return resolverstubs.GitObjectTypeCommit, nil
	case types.GitObjectTypeTag:
		return resolverstubs.GitObjectTypeTag, nil
	case types.GitObjectTypeTree:
		return resolverstubs.GitObjectTypeTree, nil
	default:
		return "", errors.Errorf("unknown git object type %s", r.configurationPolicy.Type)
	}
}

func (r *configurationPolicyResolver) Pattern() string {
	return r.configurationPolicy.Pattern
}

func (r *configurationPolicyResolver) Protected() bool {
	return r.configurationPolicy.Protected
}

func (r *configurationPolicyResolver) RetentionEnabled() bool {
	return r.configurationPolicy.RetentionEnabled
}

func (r *configurationPolicyResolver) RetentionDurationHours() *int32 {
	return toHours(r.configurationPolicy.RetentionDuration)
}

func (r *configurationPolicyResolver) RetainIntermediateCommits() bool {
	return r.configurationPolicy.RetainIntermediateCommits
}

func (r *configurationPolicyResolver) IndexingEnabled() bool {
	return r.configurationPolicy.IndexingEnabled
}

func (r *configurationPolicyResolver) IndexCommitMaxAgeHours() *int32 {
	return toHours(r.configurationPolicy.IndexCommitMaxAge)
}

func (r *configurationPolicyResolver) IndexIntermediateCommits() bool {
	return r.configurationPolicy.IndexIntermediateCommits
}
