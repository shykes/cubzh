package main

import (
	"github.com/cubzh/cubzh/.github/internal/dagger"
)

type Github struct{}

func (m *Github) Config() *dagger.Directory {
	ci := dag.
		Gha(dagger.GhaOpts{
			PublicToken: "p.eyJ1IjogIjFiZjEwMmRjLWYyZmQtNDVhNi1iNzM1LTgxNzI1NGFkZDU2ZiIsICJpZCI6ICI4ZmZmNmZkMi05MDhiLTQ4YTEtOGQ2Zi1iZWEyNGRkNzk4MTkifQ.l1Sf1gB37veXUWhxOgmjvjYcrh32NiuovbMxvjVI7Z0",
		}).
		WithPipeline(
			"debug",
			"directory with-directory --directory=. glob --pattern=*",
			dagger.GhaWithPipelineOpts{
				Dispatch: true,
				Module:   "github.com/shykes/core",
			}).
		WithPipeline(
			"Lua Modules (linter)",
			"lint-modules --src=.:modules",
			dagger.GhaWithPipelineOpts{
				Dispatch: true,
				//		SparseCheckout: []string{
				//			"lua",
				//		},
			},
		).
		WithPipeline(
			"Core Unit Tests",
			"test-core --src=.:test-core",
			dagger.GhaWithPipelineOpts{
				Dispatch: true,
				Lfs:      true,
				//		SparseCheckout: []string{
				//			"core",
				//			"deps/libz",
				//		},
			}).
		WithPipeline(
			"Core clang-format",
			"lint-core --src=.:lint-core",
			dagger.GhaWithPipelineOpts{
				Dispatch: true,
				Lfs:      true,
				//				SparseCheckout: []string{
				//					"core",
				//					"deps/libz",
				//				},
			})
	return ci.
		OnPullRequest(
			[]string{
				"Lua Modules (linter)",
				"Core Unit Tests",
				"Core clang-format",
			},
			dagger.GhaOnPullRequestOpts{
				Branches: []string{"main"},
			}).
		Config().
		Directory(".github")
}
