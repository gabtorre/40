// Code generated - EDITING IS FUTILE. DO NOT EDIT.
//
// Generated by:
//     public/app/plugins/gen.go
// Using jennies:
//     TSTypesJenny
//     PluginTSTypesJenny
//
// Run 'make gen-cue' from repository root to regenerate.

export const PanelCfgModelVersion = Object.freeze([0, 0]);

export interface PanelOptions {
  /**
   * empty/missing will default to grafana blog
   */
  feedUrl?: string;
  showImage?: boolean;
}

export const defaultPanelOptions: Partial<PanelOptions> = {
  showImage: true,
};
