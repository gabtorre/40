package keyword

import (
	"context"

	"github.com/opentracing/opentracing-go/log"

	"github.com/sourcegraph/sourcegraph/internal/search"
	"github.com/sourcegraph/sourcegraph/internal/search/job"
	"github.com/sourcegraph/sourcegraph/internal/search/query"
	"github.com/sourcegraph/sourcegraph/internal/search/streaming"
	"github.com/sourcegraph/sourcegraph/internal/trace"
)

func NewKeywordSearchJob(b query.Basic, newJob func(query.Basic) (job.Job, error)) (job.Job, error) {
	keywordQuery, err := basicQueryToKeywordQuery(b)
	if err != nil || keywordQuery == nil {
		return nil, err
	}

	child, err := newJob(keywordQuery.query)
	if err != nil {
		return nil, err
	}
	return &keywordSearchJob{child: child, patterns: keywordQuery.patterns}, nil
}

type keywordSearchJob struct {
	child    job.Job
	patterns []string
}

func (j *keywordSearchJob) Run(ctx context.Context, clients job.RuntimeClients, stream streaming.Sender) (alert *search.Alert, err error) {
	_, ctx, stream, finish := job.StartSpan(ctx, stream, j)
	defer func() { finish(alert, err) }()

	return j.child.Run(ctx, clients, stream)
}

func (j *keywordSearchJob) Name() string {
	return "KeywordSearchJob"
}

func (j *keywordSearchJob) Fields(v job.Verbosity) (res []log.Field) {
	switch v {
	case job.VerbosityMax:
		fallthrough
	case job.VerbosityBasic:
		res = append(res,
			trace.Printf("keyword", ""),
		)
	}
	return res
}

func (j *keywordSearchJob) Children() []job.Describer {
	return []job.Describer{j.child}
}

func (j *keywordSearchJob) MapChildren(fn job.MapFunc) job.Job {
	cp := *j
	cp.child = job.Map(j.child, fn)
	return &cp
}
